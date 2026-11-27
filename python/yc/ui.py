# SPDX-License-Identifier: Apache-2.0
"""Controls, charts, custom panels, containers, layout (PLAN §9, §10, §12.9).

Controls (OLEObjects) and charts (ChartObjects) on sheets are stored in the workbook; when Python changes a property
YCGrid's UI thread draws it (rule 20: Python never calls ImGui directly). Positions and sizes are always in points (pt),
like Excel.

    ws = Worksheets("Quotes")
    btn = ws.OLEObjects.Add("Forms.CommandButton.1", Left=10, Top=10, Width=80, Height=24)
    btn.Caption = "Order"                        # event: CommandButton1_Click() in sheets\\<CodeName>.py
    co = ws.ChartObjects.Add(300, 10, 360, 216)
    co.Chart.SetSourceData(ws.Range("A1:C20"))
    co.Chart.ChartType = xlLine
    s = co.Chart.SeriesCollection.NewSeries()     # [YC] draw array data directly, not through cells
    s.SetData(y=[1, 2, 3])
    p = Application.Panels.Add("Risk", Dock=ycDockRight)
    p.Controls.Add(ycButton, "Stop", Caption="Stop All", OnAction=stop_all)
"""
from __future__ import annotations

import inspect
import os
import threading
from typing import Any, Optional

from .errors import ApplicationDefinedError, SubscriptOutOfRangeError, UnsupportedError

__all__ = ["state", "screenshot", "OLEObjects", "OLEObject", "ChartObjects", "ChartObject", "Chart", "Series", "Shapes", "Shape", "Panels", "Containers", "Layout", "LoadPicture",
           "IRibbonUI", "IRibbonControl", "pane", "panes", "qat", "commands", "command", "windows"]


# ---------------------------------------------------------------------------
# RibbonX: customUI.xml in the project folder (PLAN §11.9)
# ---------------------------------------------------------------------------
class IRibbonControl:
    """The control passed to RibbonX callbacks: onAction(control), getLabel(control)..."""

    def __init__(self, id: str = "", tag: str = ""):  # noqa: A002
        self.Id = id
        self.Tag = tag
        self.Context = None

    def __repr__(self):
        return f"<IRibbonControl {self.Id}>"


class IRibbonUI:
    """Passed to customUI onLoad: Invalidate() asks every get* callback again, InvalidateControl(id) only that control's."""

    def Invalidate(self):  # noqa: N802
        _rq("ribbon.invalidate", control="")

    def InvalidateControl(self, ControlID):  # noqa: N802,N803
        _rq("ribbon.invalidate", control=str(ControlID))

    def InvalidateControlMso(self, ControlID):  # noqa: N802,N803
        pass  # built-in controls follow YCGrid's own state

    def ActivateTab(self, ControlID):  # noqa: N802,N803
        """Select a custom tab by its id."""
        return bool(_rq("ribbon.activate", id=str(ControlID)))

    def ActivateTabMso(self, ControlID):  # noqa: N802,N803
        """Select a built-in tab: "TabHome", "TabInsert", "TabData"..."""
        return bool(_rq("ribbon.activate", id=str(ControlID)))


def _rq(method: str, **params) -> Any:
    from ._conn import current

    return current().request(method, params)


def state() -> dict:
    """[YC] What YCGrid shows, without a screenshot: sheets, active sheet / cell / selection and its value, the open
    dialog, the status text, operator mode, safety alarms, bound tables, figures, replay, the Python host, and the
    undo / redo lists ({"depth", "label"}: how many steps, the next one's name)."""
    return _rq("ui.state")


def ping() -> bool:
    """[YC] True when YCGrid's UI thread answers. Unlike state(), it doesn't wait for the calculation engine's queue
    first: a slow answer is a frozen screen, not a busy engine (watchdogs, tests)."""
    return bool(_rq("ui.ping"))


def errors(errors_only: bool = False, after: int = 0) -> list:
    """[YC] What the Error Log window lists (View > Error Log), oldest first: dicts with id, time (Unix seconds), source
    ("YCGrid", "Python", "VS Code"...), title, text and error (False: a message YCGrid showed), and refusal (True: what
    a protected sheet or workbook refused, which Excel says in a message box). after: only newer ids."""
    return list(_rq("errors.list", errors_only=bool(errors_only), after=int(after)))


def clear_errors() -> None:
    """[YC] Empty the Error Log window (the log file keeps everything)."""
    _rq("errors.clear")


def resolve_errors(ids, note: str = "") -> int:
    """[YC] Mark errors (ids from errors()) as no longer applying: they stay in the Error Log as messages with the note
    added, and stop counting in the status bar's red error chip. Returns how many changed. YCGrid does this itself for
    "No module named ..." errors once the module is installed."""
    return int(_rq("errors.resolve", ids=[int(i) for i in ids], note=str(note)))


def items(window: str = "") -> list:
    """[YC] Tests: the labelled controls on the screen now (buttons, check boxes, radio buttons, text boxes, menu items,
    list entries...) as dicts {label, id, window, rect [x0, y0, x1, y1], disabled} (id: the label with its "##" part,
    for controls with no visible text), and for a check box or a checkable menu item `checked`: True, False or "mixed"
    (a (Select All) over some ticked items); window filters by part of the window title and then lists its unlabelled
    controls too (color swatches, drawn buttons: label and id ""), in no particular order. Click one with ui.input
    "c:x,y" as a person does. The first call turns the recording on."""
    got = _rq("ui.items", window=str(window))
    if got is None:  # recording just started: let a frame be drawn
        _rq("ui.input", script="w:2")
        got = _rq("ui.items", window=str(window))
    return list(got or [])


def ribbon_items() -> list:
    """[YC] Tests: the ribbon buttons, open menu entries, gallery tiles and Backstage entries on the screen now, as
    dicts {cmd, where, rect [x0, y0, x1, y1] in window pixels}: where is "ribbon" (a button), "dropdown" (opens a menu
    or gallery: click its arrow), "combo", "menu" (an entry of the open menu), "tile" or "backstage". Click them
    with ui.input "c:x,y" as a person does. Only what the last frames drew: switch to a tab or open a menu first."""
    return list(_rq("ui.ribbon_items"))


def answer_file_dialogs(*paths: str) -> None:
    """[YC] Tests: what the next file dialogs of a hidden YCGrid choose, in order (Open, Save As, Insert Picture,
    Export PDF, Replay, Save Screenshot, Change Source, GetOpenFilename...); "" is Cancel. A hidden YCGrid cancels a
    file dialog with no answer queued. Each answer is listed in errors() as a message."""
    _rq("ui.answer_file_dialogs", paths=[os.path.abspath(p) if p else "" for p in paths])


def errors_folder() -> str:
    """[YC] The folder of the daily error log files (errors-YYYY-MM-DD.log); "" when they aren't written."""
    return str(_rq("errors.folder"))


def pane(name: str, visible: Optional[bool] = None) -> bool:
    """[YC] Show or hide a built-in pane, or read whether it shows: "immediate", "watch", "errors", "clipboard",
    "comments", "pivot_fields", "latency", "alerts", "heartbeats", "change_log", "chart_window", "perf",
    "formula_bar" (panes()). Returns whether it shows."""
    p = {"name": str(name)}
    if visible is not None:
        p["visible"] = bool(visible)
    return bool(_rq("cmdbar.pane", **p))


def panes() -> dict:
    """[YC] {pane name: whether it shows} for every built-in pane."""
    return dict(_rq("cmdbar.pane", name=""))


def qat(commands: Optional[list] = None) -> list:
    """[YC] The command ids on the Quick Access Toolbar; with commands, set them (saved for this user)."""
    if commands is None:
        return list(_rq("cmdbar.qat"))
    return list(_rq("cmdbar.qat", ids=[str(c) for c in commands]))


def commands() -> list:
    """[YC] Every UI command: id, label, shortcut, toggle, pressed, enabled, ribbon (shown as a button on a built-in ribbon
    tab), icon (that button, or the command elsewhere, has an icon) and glyph (its icon's character, "" for none).
    Run one with command(id)."""
    return list(_rq("cmdbar.commands"))


def command(id: str) -> bool:  # noqa: A002
    """[YC] Run a UI command by id, as its ribbon button, menu item or shortcut does (the same as
    Application.CommandBars.ExecuteMso(id)); returns False when the command doesn't exist or is disabled."""
    return bool(_rq("ui.command", id=str(id)))


def windows() -> list:
    """[YC] The windows on the screen (plot figures, panels, containers, the Latency Monitor, panes...): label, name,
    rect = [left, top, right, bottom] in the window pixels ui.input takes, and docked; a docked one also has tab (the
    rect of its own tab, where a user grabs it: in a group of tabs the first one may be another window's) and
    tab_selected, and floating (True when its group floats over the sheets as a window of its own). The other tabs
    of a group are listed too, with visible False (rect = the group's).
    screenshot(target="window:<label>") takes a picture of one."""
    return list(_rq("image.windows"))


def cell_rect(address: str) -> list:
    """[YC] Where a range of the active sheet is on the screen: [left, top, right, bottom] in the window pixels
    ui.input takes ("m:x,y", "d:x1,y1,x2,y2"), scrolled into view first. For drags a person makes: a selection's
    border, the fill handle (the bottom-right corner), a column's border in the header row."""
    return [float(v) for v in _rq("ui.cell_rect", address=str(address))]


def screenshot(path: Optional[str] = None, target: str = "window", address: Optional[str] = None, max_width: int = 1600) -> dict:
    """[YC] A picture of what is on the screen: the whole window ("window"), the grid ("grid"), the visible part of a
    range (target="range", address="A1:H20") or one floating window (target="window:<label>", see windows()), as a
    PNG at path, or on the Clipboard when path is None. Works with --hidden too. Returns ok, width, height and what
    was visible. For cells that don't fit on the screen use Range / Worksheet / Workbook.ExportImage."""
    import os

    p = {"target": target, "max_width": int(max_width)}
    if target.startswith("window:"):
        label = target[7:]
        match = [w for w in windows() if w["label"] == label] or [w for w in windows() if w["label"].startswith(label)]
        if not match:
            raise ApplicationDefinedError(f"No window {label!r} on the screen ({', '.join(w['label'] for w in windows())})")
        p["target"], p["address"] = "imgui_window", match[0]["name"]
    elif address:
        p["address"] = address
    if path is None:
        p["path"], p["to_clipboard"] = "", True
    else:
        p["path"] = os.path.abspath(path)
    return _rq("ui.screenshot", **p)


def _series_values(text: str) -> tuple:
    """A series' values from the text that holds them: a range ("Sheet1!$B$2:$B$5"), an array ("{1,2,3}") or a name."""
    from . import excel

    text = (text or "").lstrip("=")
    if not text:
        return ()
    try:
        if text.startswith("{"):
            v = excel.Evaluate(text)
        else:
            v = excel.Range(text).Value2
    except Exception:  # noqa: BLE001  (a reference to a deleted sheet: no values, as Excel's #REF! series)
        return ()
    if isinstance(v, list):
        return tuple(x for row in v for x in (row if isinstance(row, list) else [row]))
    return (v,)


def _addr(v) -> str:
    """Range object or string -> address including the sheet name."""
    if v is None:
        return ""
    if isinstance(v, str):
        return v
    addr = getattr(v, "Address", None)
    if addr is not None:
        return addr(External=True)
    raise ApplicationDefinedError(f"A range or an address string is required: {v!r}")


def _excel_color(v) -> int:
    return int(v) & 0xFFFFFF


def _argb(v) -> int:
    """Excel color (R + G*256 + B*65536) -> ARGB."""
    v = int(v) & 0xFFFFFF
    return 0xFF000000 | ((v & 0xFF) << 16) | (v & 0xFF00) | (v >> 16)


def _from_argb(v) -> int:
    v = int(v)
    return ((v >> 16) & 0xFF) | (v & 0xFF00) | ((v & 0xFF) << 16)


# ---------------------------------------------------------------------------
# controls (ActiveX compatible: Forms.CommandButton.1, Forms.ComboBox.1, Forms.CheckBox.1, Forms.OptionButton.1,
# Forms.ToggleButton.1, Forms.TextBox.1, Forms.Label.1, Forms.ListBox.1, Forms.SpinButton.1, Forms.ScrollBar.1, Forms.Image.1)
# ---------------------------------------------------------------------------
_FM_LIST = 2  # fmStyleDropDownList
_KINDS = {"commandbutton": "button", "combobox": "combobox", "checkbox": "checkbox", "optionbutton": "optionbutton",
          "togglebutton": "togglebutton", "textbox": "textbox", "label": "label", "listbox": "listbox", "spinbutton": "spinbutton",
          "scrollbar": "scrollbar", "image": "image", "groupbox": "groupbox"}
# default size (points) of a new control of each kind
_SIZES = {"button": (72, 24), "combobox": (96, 18), "checkbox": (108, 18), "optionbutton": (108, 18), "togglebutton": (72, 24),
          "textbox": (96, 18), "label": (72, 18), "listbox": (96, 72), "spinbutton": (13, 26), "scrollbar": (13, 72), "image": (72, 72),
          "groupbox": (144, 96)}
_BOOL_KINDS = ("checkbox", "optionbutton", "togglebutton")


def _kind_of(class_type) -> str:
    """'Forms.CheckBox.1' / 'CheckBox' / 'checkbox' -> the kind name used by YCGrid."""
    t = str(class_type).strip().lower()
    if t.startswith("forms."):
        t = t[6:]
    t = t.split(".")[0]
    if t in _KINDS:
        return _KINDS[t]
    if t in _KINDS.values():
        return t
    raise UnsupportedError(f"Unsupported control class: {class_type}")


class _Picture:
    """LoadPicture(): picture bytes for Image.Picture."""

    def __init__(self, data: bytes):
        self.data = bytes(data)


def LoadPicture(Filename):  # noqa: N802,N803
    with open(str(Filename), "rb") as f:
        return _Picture(f.read())


def _picture_bytes(v) -> bytes:
    if v is None:
        return b""
    if isinstance(v, _Picture):
        return v.data
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    return LoadPicture(v).data


class _Selected:
    """ListBox.Selected(i) / Selected[i] (0-based, like Excel)."""

    def __init__(self, ctl):
        self._c = ctl

    def __call__(self, i):
        return int(i) in self._c._get()["selected"]

    __getitem__ = __call__

    def __setitem__(self, i, v):
        sel = set(self._c._get()["selected"])
        if v:
            sel.add(int(i))
        else:
            sel.discard(int(i))
        self._c._set(selected=sorted(sel))


class _Font:
    def __init__(self, ctl: "OLEObject"):
        self._c = ctl

    @property
    def Size(self):  # noqa: N802
        return self._c._get()["font_size"] or 11

    @Size.setter
    def Size(self, v):  # noqa: N802
        self._c._set(font_size=float(v))

    @property
    def Bold(self):  # noqa: N802
        return self._c._get()["bold"]

    @Bold.setter
    def Bold(self, v):  # noqa: N802
        self._c._set(bold=bool(v))

    @property
    def Italic(self):  # noqa: N802
        return self._c._get()["italic"]

    @Italic.setter
    def Italic(self, v):  # noqa: N802
        self._c._set(italic=bool(v))

    @property
    def Name(self):  # noqa: N802
        return self._c._get()["font_name"]

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._c._set(font_name=str(v))


class OLEObject:
    """A control on a sheet. Excel's OLEObject and the MSForms control (.Object) are merged into one object."""

    def __init__(self, sheet_id: int, name: str):
        self.__dict__["_sheet"] = sheet_id
        self.__dict__["_name"] = name

    def __repr__(self):
        return f"<OLEObject {self._name}>"

    def __eq__(self, other):
        return isinstance(other, OLEObject) and other._sheet == self._sheet and other._name.lower() == self._name.lower()

    def __hash__(self):
        return hash((self._sheet, self._name.lower()))

    def _get(self) -> dict:
        return _rq("ctl.get", sheet=self._sheet, name=self._name)

    def _set(self, **props) -> dict:
        d = _rq("ctl.set", sheet=self._sheet, name=self._name, props=props)
        if d and "name" in props:
            self.__dict__["_name"] = d["name"]
        return d

    # ---- Common ----
    @property
    def Name(self):  # noqa: N802
        return self._name

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._set(name=str(v))

    @property
    def Object(self):  # noqa: N802
        return self

    @property
    def progID(self):  # noqa: N802
        return self._get()["prog_id"]

    ProgID = progID

    @property
    def Parent(self):  # noqa: N802
        from . import excel

        return excel.Worksheet(self._sheet)

    def _geom(name):  # noqa: N805
        def g(self):
            return self._get()[name]

        def s(self, v):
            self._set(**{name: float(v)})

        return property(g, s)

    Left = _geom("left")
    Top = _geom("top")
    Width = _geom("width")
    Height = _geom("height")
    del _geom

    @property
    def Caption(self):  # noqa: N802
        return self._get()["caption"]

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        self._set(caption=str(v))

    @property
    def Enabled(self):  # noqa: N802
        return self._get()["enabled"]

    @Enabled.setter
    def Enabled(self, v):  # noqa: N802
        self._set(enabled=bool(v))

    @property
    def Visible(self):  # noqa: N802
        return self._get()["visible"]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._set(visible=bool(v))

    @property
    def Locked(self):  # noqa: N802
        return self._get()["locked"]

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        self._set(locked=bool(v))

    @property
    def PrintObject(self):  # noqa: N802
        """Format Control > Properties > Print object: False leaves the control out of the printout."""
        return bool(self._get().get("print_object", True))

    @PrintObject.setter
    def PrintObject(self, v):  # noqa: N802
        self._set(print_object=bool(v))

    @property
    def AlternativeText(self):  # noqa: N802
        """Format Control > Alt Text."""
        return self._get().get("alt_text", "")

    @AlternativeText.setter
    def AlternativeText(self, v):  # noqa: N802
        self._set(alt_text=str(v))

    @property
    def Placement(self):  # noqa: N802
        return self._get()["placement"]

    @Placement.setter
    def Placement(self, v):  # noqa: N802
        self._set(placement=int(v))

    @property
    def BackColor(self):  # noqa: N802
        return self._get()["back_color"]

    @BackColor.setter
    def BackColor(self, v):  # noqa: N802
        self._set(back_color=_excel_color(v))

    @property
    def ForeColor(self):  # noqa: N802
        return self._get()["fore_color"]

    @ForeColor.setter
    def ForeColor(self, v):  # noqa: N802
        self._set(fore_color=_excel_color(v))

    @property
    def Font(self):  # noqa: N802
        return _Font(self)

    @property
    def ZOrderPosition(self):  # noqa: N802
        return self._get()["z"]

    def BringToFront(self):  # noqa: N802
        lst = _list(self._sheet)
        z = max([c["z"] for c in lst["controls"]] + [c["z"] for c in lst["charts"]] + [0])
        _rq("ctl.set", sheet=self._sheet, name=self._name, props={"z": z + 1})

    def SendToBack(self):  # noqa: N802
        lst = _list(self._sheet)
        z = min([c["z"] for c in lst["controls"]] + [c["z"] for c in lst["charts"]] + [0])
        _rq("ctl.set", sheet=self._sheet, name=self._name, props={"z": z - 1})

    def Delete(self):  # noqa: N802
        _rq("ctl.delete", sheet=self._sheet, name=self._name)

    def Select(self, Replace=True):  # noqa: N802,N803
        pass

    # ---- ComboBox ----
    @property
    def LinkedCell(self):  # noqa: N802
        return self._get()["linked_cell"]

    @LinkedCell.setter
    def LinkedCell(self, v):  # noqa: N802
        self._set(linked_cell=_addr(v))

    @property
    def ListFillRange(self):  # noqa: N802
        return self._get()["list_fill_range"]

    @ListFillRange.setter
    def ListFillRange(self, v):  # noqa: N802
        self._set(list_fill_range=_addr(v))

    def _items(self) -> list:
        d = self._get()
        if d["list_fill_range"]:
            from . import excel

            vals = excel.Range(d["list_fill_range"]).Value
            if not isinstance(vals, (list, tuple)):
                return ["" if vals is None else str(vals)]
            if isinstance(vals, (list, tuple)):
                out = []
                for row in vals:
                    if isinstance(row, (list, tuple)):
                        out.extend("" if x is None else str(x) for x in row)
                    else:
                        out.append("" if row is None else str(row))
                return out
        return list(d["items"])

    @property
    def List(self):  # noqa: N802
        return self._items()

    @List.setter
    def List(self, v):  # noqa: N802
        self._set(items=["" if x is None else str(x) for x in v], list_fill_range="")

    def AddItem(self, Item, Index=None):  # noqa: N802,N803
        items = list(self._get()["items"])
        if Index is None:
            items.append(str(Item))
        else:
            items.insert(int(Index), str(Item))
        self._set(items=items)

    def RemoveItem(self, Index):  # noqa: N802,N803
        items = list(self._get()["items"])
        try:
            items.pop(int(Index))
        except IndexError:
            raise ApplicationDefinedError("RemoveItem: index out of range") from None
        self._set(items=items)

    def Clear(self):  # noqa: N802
        self._set(items=[], value="", list_index=-1)

    @property
    def ListCount(self):  # noqa: N802
        return len(self._items())

    @property
    def ListIndex(self):  # noqa: N802
        d = self._get()
        items = self._items()
        return items.index(d["value"]) if d["value"] in items else -1

    @ListIndex.setter
    def ListIndex(self, v):  # noqa: N802
        items = self._items()
        v = int(v)
        if v < -1 or v >= len(items):
            raise ApplicationDefinedError("ListIndex out of range")
        self._set(value=items[v] if v >= 0 else "", list_index=v)

    @property
    def Value(self):  # noqa: N802
        """CheckBox / OptionButton / ToggleButton: True or False; SpinButton / ScrollBar: a number; TextBox, ComboBox, ListBox: the text."""
        d = self._get()
        kind = d["kind"]
        if kind in ("button", "image"):
            return False
        if kind == "label":
            return d["caption"]
        if d["linked_cell"]:
            from . import excel

            v = excel.Range(d["linked_cell"]).Value
        else:
            v = d["value"]
        if kind in _BOOL_KINDS:
            return v in (True, 1, -1) or str(v).lower() in ("true", "1", "-1")
        if kind in ("spinbutton", "scrollbar"):
            try:
                return int(float(v))
            except (TypeError, ValueError):
                return d["min"]
        if kind == "listbox" and d["multi_select"]:
            return None
        return v

    @Value.setter
    def Value(self, v):  # noqa: N802
        kind = self._get()["kind"]
        if kind in _BOOL_KINDS:
            self._set(value="True" if v in (True, 1, -1) or str(v).lower() == "true" else "False")
        elif kind in ("spinbutton", "scrollbar"):
            self._set(value=str(int(v)))
        elif kind == "label":
            self._set(caption=str(v))
        else:
            self._set(value="" if v is None else str(v))

    # ---- OptionButton ----
    @property
    def GroupName(self):  # noqa: N802
        return self._get()["group_name"]

    @GroupName.setter
    def GroupName(self, v):  # noqa: N802
        self._set(group_name=str(v or ""))

    # ---- SpinButton, ScrollBar ----
    def _int_prop(key):  # noqa: N805
        def g(self):
            return self._get()[key]

        def s(self, v):
            self._set(**{key: int(v)})

        return property(g, s)

    Min = _int_prop("min")
    Max = _int_prop("max")
    SmallChange = _int_prop("small_change")
    LargeChange = _int_prop("large_change")
    del _int_prop

    @property
    def Orientation(self):  # noqa: N802
        """fmOrientationAuto (-1), fmOrientationVertical (0), fmOrientationHorizontal (1)."""
        return self._get()["orientation"]

    @Orientation.setter
    def Orientation(self, v):  # noqa: N802
        self._set(orientation=int(v))

    # ---- ListBox ----
    @property
    def MultiSelect(self):  # noqa: N802
        """fmMultiSelectSingle (0), fmMultiSelectMulti (1), fmMultiSelectExtended (2)."""
        return self._get()["multi_select"]

    @MultiSelect.setter
    def MultiSelect(self, v):  # noqa: N802
        self._set(multi_select=int(v))

    @property
    def Selected(self):  # noqa: N802
        return _Selected(self)

    # ---- TextBox, Label ----
    @property
    def MultiLine(self):  # noqa: N802
        return self._get()["multi_line"]

    @MultiLine.setter
    def MultiLine(self, v):  # noqa: N802
        self._set(multi_line=bool(v))

    @property
    def PasswordChar(self):  # noqa: N802
        return self._get()["password_char"]

    @PasswordChar.setter
    def PasswordChar(self, v):  # noqa: N802
        self._set(password_char=str(v or "")[:1])

    @property
    def WordWrap(self):  # noqa: N802
        return self._get()["word_wrap"]

    @WordWrap.setter
    def WordWrap(self, v):  # noqa: N802
        self._set(word_wrap=bool(v))

    # ---- Image ----
    @property
    def Picture(self):  # noqa: N802
        """Set with LoadPicture(path), a file path or bytes; reads True when there is a picture."""
        return self._get()["has_picture"]

    @Picture.setter
    def Picture(self, v):  # noqa: N802
        self._set(picture=_picture_bytes(v))

    @property
    def PictureSizeMode(self):  # noqa: N802
        """fmPictureSizeModeClip (0), fmPictureSizeModeStretch (1), fmPictureSizeModeZoom (3)."""
        return self._get()["picture_size_mode"]

    @PictureSizeMode.setter
    def PictureSizeMode(self, v):  # noqa: N802
        self._set(picture_size_mode=int(v))

    @property
    def Text(self):  # noqa: N802
        d = self._get()
        if d["linked_cell"]:
            from . import excel

            return excel.Range(d["linked_cell"]).Text
        return d["value"]

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._set(value=str(v))

    @property
    def Style(self):  # noqa: N802
        return _FM_LIST if self._get()["style"] == 1 else 0

    @Style.setter
    def Style(self, v):  # noqa: N802
        self._set(style=1 if int(v) == _FM_LIST else 0)

    @property
    def ListRows(self):  # noqa: N802
        return self._get()["list_rows"]

    @ListRows.setter
    def ListRows(self, v):  # noqa: N802
        self._set(list_rows=int(v))

    def __setattr__(self, name, value):
        if name.startswith("_") or hasattr(type(self), name):
            object.__setattr__(self, name, value)
        else:
            raise AttributeError(f"The control has no property {name}")


def _list(sheet_id: int) -> dict:
    return _rq("obj.list", sheet=sheet_id)


def find_control(sheet_id: int, name: str) -> Optional[OLEObject]:
    """Me.CommandButton1: find a control by name (None when not found)."""
    try:
        lst = _list(sheet_id)
    except Exception:  # noqa: BLE001
        return None
    for c in lst["controls"]:
        if c["name"].lower() == name.lower():
            return OLEObject(sheet_id, c["name"])
    return None


class OLEObjects:
    def __init__(self, sheet_id: int):
        self._sheet = sheet_id

    def _names(self):
        # form controls are in Shapes / Buttons / DropDowns..., not in OLEObjects (like Excel)
        return [c["name"] for c in _list(self._sheet)["controls"] if not c.get("form")]

    def Add(self, ClassType="Forms.CommandButton.1", Link=False, DisplayAsIcon=False, IconFileName=None, IconIndex=None,  # noqa: N802,N803
            IconLabel=None, Left=0, Top=0, Width=None, Height=None, Name=None, Caption=None, Placement=None, **props):
        kind = _kind_of(ClassType)
        w, h = _SIZES[kind]
        p = {"kind": kind, "left": float(Left), "top": float(Top),
             "width": float(Width) if Width is not None else float(w),
             "height": float(Height) if Height is not None else float(h)}
        if kind == "label":
            p["word_wrap"] = True
        if Name:
            p["name"] = str(Name)
        if Caption is not None:
            p["caption"] = str(Caption)
        if Placement is not None:
            p["placement"] = int(Placement)
        for k, v in props.items():  # LinkedCell=, ListFillRange=, List=
            lk = k.lower()
            if lk == "linkedcell":
                p["linked_cell"] = _addr(v)
            elif lk == "listfillrange":
                p["list_fill_range"] = _addr(v)
            elif lk in ("list", "items"):
                p["items"] = [str(x) for x in v]
            elif lk == "value":
                p["value"] = ("True" if v in (True, 1, -1) else "False") if kind in _BOOL_KINDS else str(v)
            elif lk == "style":
                p["style"] = 1 if int(v) == _FM_LIST else 0
            elif lk == "groupname":
                p["group_name"] = str(v)
            elif lk in ("min", "max", "smallchange", "largechange", "orientation", "multiselect", "picturesizemode"):
                p[{"min": "min", "max": "max", "smallchange": "small_change", "largechange": "large_change", "orientation": "orientation",
                   "multiselect": "multi_select", "picturesizemode": "picture_size_mode"}[lk]] = int(v)
            elif lk in ("multiline", "wordwrap"):
                p["multi_line" if lk == "multiline" else "word_wrap"] = bool(v)
            elif lk == "passwordchar":
                p["password_char"] = str(v)[:1]
            elif lk == "picture":
                p["picture"] = _picture_bytes(v)
            else:
                raise ApplicationDefinedError(f"OLEObjects.Add: unknown property {k}")
        d = _rq("ctl.add", sheet=self._sheet, **p)
        return OLEObject(self._sheet, d["name"])

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Item(self, Index):  # noqa: N802,N803
        names = self._names()
        if isinstance(Index, int):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError("OLEObjects index out of range")
            return OLEObject(self._sheet, names[Index - 1])
        for n in names:
            if n.lower() == str(Index).lower():
                return OLEObject(self._sheet, n)
        raise SubscriptOutOfRangeError(f"Control not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([OLEObject(self._sheet, n) for n in self._names()])

    def __len__(self):
        return self.Count

    def Delete(self):  # noqa: N802
        for n in self._names():
            _rq("ctl.delete", sheet=self._sheet, name=n)


# ---------------------------------------------------------------------------
# form controls (Developer -> Insert -> Form Controls; Shapes.AddFormControl, Buttons, DropDowns...)
# ---------------------------------------------------------------------------
_FORM_TYPES = {0: "button", 1: "checkbox", 2: "combobox", 4: "groupbox", 5: "label", 6: "listbox", 7: "optionbutton", 8: "scrollbar",
               9: "spinbutton"}
_FORM_TYPES_OUT = {v: k for k, v in _FORM_TYPES.items()}
_FORM_NAMES = {"button": "Button", "checkbox": "Check Box", "combobox": "Drop Down", "groupbox": "Group Box", "label": "Label",
               "listbox": "List Box", "optionbutton": "Option Button", "scrollbar": "Scroll Bar", "spinbutton": "Spinner"}
_XL_ON, _XL_OFF = 1, -4146


def _add_form(sheet_id, kind, Left, Top, Width, Height):  # noqa: N803
    n = 1 + sum(1 for c in _list(sheet_id)["controls"] if c.get("form") and c["kind"] == kind)
    p = {"kind": kind, "form": True, "left": float(Left), "top": float(Top), "width": float(Width), "height": float(Height),
         "name": f"{_FORM_NAMES[kind]} {n}"}
    if kind in ("button", "checkbox", "optionbutton", "groupbox", "label"):
        p["caption"] = p["name"]
    if kind == "label":
        p["word_wrap"] = True
    d = _rq("ctl.add", sheet=sheet_id, **p)
    return FormControl(sheet_id, d["name"])


class _Characters:
    def __init__(self, ctl):
        self.__dict__["_c"] = ctl

    @property
    def Text(self):  # noqa: N802
        return self._c.Caption

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._c.Caption = v

    def __setattr__(self, name, value):
        object.__setattr__(self, name, value)


class _FormTextFrame:
    def __init__(self, ctl):
        self._c = ctl

    def Characters(self, Start=None, Length=None):  # noqa: N802,N803
        return _Characters(self._c)

    # Format Control > Margins (a form button): AutoMargins, MarginLeft / Top / Right / Bottom in points
    @property
    def AutoMargins(self):  # noqa: N802
        return bool(self._c._get().get("auto_margins", True))

    @AutoMargins.setter
    def AutoMargins(self, v):  # noqa: N802
        self._c._set(auto_margins=bool(v))

    def _m(key):  # noqa: N805
        def g(self):
            return self._c._get().get(key, 0.0)

        def s(self, v):
            self._c._set(**{key: float(v), "auto_margins": False})

        return property(g, s)

    MarginLeft = _m("margin_l")
    MarginTop = _m("margin_t")
    MarginRight = _m("margin_r")
    MarginBottom = _m("margin_b")
    del _m


_H_ALIGN = {0: -4131, 1: -4108, 2: -4152}  # xlLeft, xlCenter, xlRight
_V_ALIGN = {0: -4160, 1: -4108, 2: -4107}  # xlTop, xlCenter, xlBottom
_ORIENT = {0: -4128, 1: -4166, 2: -4171, 3: -4170}  # xlHorizontal, xlVertical, xlUpward, xlDownward


class FormControl(OLEObject):
    """A form control: Excel's Shape (Type msoFormControl) with ControlFormat and OnAction, and the old Button,
    CheckBox, DropDown, ListBox, OptionButton, Spinner, ScrollBar, GroupBox and Label objects (same members)."""

    def __repr__(self):
        return f"<FormControl {self._name}>"

    @property
    def Type(self):  # noqa: N802
        return 8  # msoFormControl

    @property
    def FormControlType(self):  # noqa: N802
        return _FORM_TYPES_OUT.get(self._get()["kind"], 0)

    @property
    def ControlFormat(self):  # noqa: N802
        return self

    @property
    def OnAction(self):  # noqa: N802
        return self._get()["on_action"]

    @OnAction.setter
    def OnAction(self, v):  # noqa: N802
        self._set(on_action=getattr(v, "__name__", str(v or "")))

    @property
    def Text(self):  # noqa: N802
        return self._get()["caption"]

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._set(caption=str(v))

    @property
    def TextFrame(self):  # noqa: N802
        return _FormTextFrame(self)

    # Format Control > Alignment (a form button), and 3-D shading (check boxes, option buttons, lists, group boxes,
    # scroll bars, spinners), by Excel's names
    @property
    def HorizontalAlignment(self):  # noqa: N802
        return _H_ALIGN.get(self._get().get("text_halign", 1), -4108)

    @HorizontalAlignment.setter
    def HorizontalAlignment(self, v):  # noqa: N802
        self._set(text_halign={b: a for a, b in _H_ALIGN.items()}.get(int(v), 1))

    @property
    def VerticalAlignment(self):  # noqa: N802
        return _V_ALIGN.get(self._get().get("text_valign", 1), -4108)

    @VerticalAlignment.setter
    def VerticalAlignment(self, v):  # noqa: N802
        self._set(text_valign={b: a for a, b in _V_ALIGN.items()}.get(int(v), 1))

    @property
    def Orientation(self):  # noqa: N802
        return _ORIENT.get(self._get().get("text_orientation", 0), -4128)

    @Orientation.setter
    def Orientation(self, v):  # noqa: N802
        self._set(text_orientation={b: a for a, b in _ORIENT.items()}.get(int(v), 0))

    @property
    def AutoSize(self):  # noqa: N802
        return bool(self._get().get("auto_size", False))

    @AutoSize.setter
    def AutoSize(self, v):  # noqa: N802
        self._set(auto_size=bool(v))

    @property
    def Display3DShading(self):  # noqa: N802
        return bool(self._get().get("shading_3d", False))

    @Display3DShading.setter
    def Display3DShading(self, v):  # noqa: N802
        self._set(shading_3d=bool(v))

    @property
    def LinkedCell(self):  # noqa: N802
        return self._get()["linked_cell"]

    @LinkedCell.setter
    def LinkedCell(self, v):  # noqa: N802
        self._set(linked_cell=_addr(v))

    @property
    def ListFillRange(self):  # noqa: N802
        return self._get()["list_fill_range"]

    @ListFillRange.setter
    def ListFillRange(self, v):  # noqa: N802
        self._set(list_fill_range=_addr(v))

    @property
    def Value(self):  # noqa: N802
        """Check boxes and option buttons: xlOn / xlOff; spinners and scroll bars: the number; lists: the item number."""
        d = self._get()
        k = d["kind"]
        if k in ("checkbox", "optionbutton"):
            return _XL_ON if str(d["value"]).lower() in ("true", "1", "-1") else _XL_OFF
        if k in ("combobox", "listbox"):
            return d["list_index"] + 1 if d["list_index"] >= 0 else 0
        try:
            return int(float(d["value"]))
        except (TypeError, ValueError):
            return d["min"]

    @Value.setter
    def Value(self, v):  # noqa: N802
        k = self._get()["kind"]
        if k in ("checkbox", "optionbutton"):
            self._set(value="True" if v in (True, _XL_ON) else "False")
        elif k in ("combobox", "listbox"):
            self.ListIndex = v
        else:
            self._set(value=str(int(v)))

    @property
    def ListIndex(self):  # noqa: N802
        i = self._get()["list_index"]
        return i + 1 if i >= 0 else 0

    @ListIndex.setter
    def ListIndex(self, v):  # noqa: N802
        self._set(list_index=int(v) - 1)

    def _num(name):  # noqa: N805
        def g(self):
            return self._get()[name]

        def s(self, v):
            self._set(**{name: int(v)})

        return property(g, s)

    Min = _num("min")
    Max = _num("max")
    SmallChange = _num("small_change")
    LargeChange = _num("large_change")
    DropDownLines = _num("list_rows")
    del _num

    @property
    def MultiSelect(self):  # noqa: N802
        return {0: -4142, 1: 1, 2: 3}[self._get()["multi_select"]]  # xlNone, xlSimple, xlExtended

    @MultiSelect.setter
    def MultiSelect(self, v):  # noqa: N802
        self._set(multi_select={-4142: 0, 1: 1, 3: 2}.get(int(v), 0))

    @property
    def List(self):  # noqa: N802
        d = self._get()
        if d["list_fill_range"]:
            from . import excel

            return tuple(str(c.Text) for c in excel.Range(d["list_fill_range"]))
        return tuple(d["items"])

    @List.setter
    def List(self, v):  # noqa: N802
        self._set(items=[str(x) for x in v], list_fill_range="")

    @property
    def ListCount(self):  # noqa: N802
        return len(self.List)

    def AddItem(self, Text, Index=None):  # noqa: N802,N803
        items = list(self._get()["items"])
        if Index is None:
            items.append(str(Text))
        else:
            items.insert(int(Index) - 1, str(Text))
        self._set(items=items, list_fill_range="")

    def RemoveItem(self, Index, Count=1):  # noqa: N802,N803
        items = list(self._get()["items"])
        del items[int(Index) - 1:int(Index) - 1 + int(Count)]
        self._set(items=items)

    def RemoveAllItems(self):  # noqa: N802
        self._set(items=[], list_index=-1)

    def Delete(self):  # noqa: N802
        _rq("ctl.delete", sheet=self._sheet, name=self._name)


class FormControls:
    """Worksheet.Buttons() / CheckBoxes() / DropDowns() / ListBoxes() / OptionButtons() / Spinners() / ScrollBars() /
    GroupBoxes() / Labels(): the form controls of one kind."""

    def __init__(self, sheet_id: int, kind: str):
        self._sheet, self._kind = sheet_id, kind

    def _names(self):
        return [c["name"] for c in _list(self._sheet)["controls"] if c.get("form") and c["kind"] == self._kind]

    def Add(self, Left, Top, Width, Height, Editable=None):  # noqa: N802,N803
        return _add_form(self._sheet, self._kind, Left, Top, Width, Height)

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        names = self._names()
        if isinstance(Index, int):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError("Index out of range")
            return FormControl(self._sheet, names[Index - 1])
        for n in names:
            if n.lower() == str(Index).lower():
                return FormControl(self._sheet, n)
        raise SubscriptOutOfRangeError(f"Form control not found: {Index}")

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([FormControl(self._sheet, n) for n in self._names()])

    def Delete(self):  # noqa: N802
        for n in self._names():
            _rq("ctl.delete", sheet=self._sheet, name=n)


# ---------------------------------------------------------------------------
# charts
# ---------------------------------------------------------------------------
_XL_TO_TYPE = {4: 0, 65: 1, 51: 2, 52: 3, 57: 4, -4169: 5, 1: 6, 5: 7, -4120: 8, 89: 9,
               -4151: 10, 81: 11, 82: 12, 119: 13, 118: 14, 122: 15, 121: 16, 117: 17, 120: 18, 123: 19,
               -4100: 20, 54: 21, 55: 22, 60: 23, -4101: 24, -4098: 25, -4102: 26, 83: 27, 84: 28, 85: 29, 86: 29, 140: 30,
               53: 31, 58: 32, 59: 33, 63: 34, 64: 35, 66: 36, 67: 37, 76: 38, 77: 39, 74: 40, 75: 41, 72: 42, 73: 43, 15: 44, 88: 45,
               68: 46, 71: 47, 90: 48, 91: 49}
_TYPE_TO_XL = {0: 4, 1: 65, 2: 51, 3: 52, 4: 57, 5: -4169, 6: 1, 7: 5, 8: -4120, 9: 89,
               10: -4151, 11: 81, 12: 82, 13: 119, 14: 118, 15: 122, 16: 121, 17: 117, 18: 120, 19: 123,
               20: -4100, 21: 54, 22: 55, 23: 60, 24: -4101, 25: -4098, 26: -4102, 27: 83, 28: 84, 29: 85, 30: 140,
               31: 53, 32: 58, 33: 59, 34: 63, 35: 64, 36: 66, 37: 67, 38: 76, 39: 77, 40: 74, 41: 75, 42: 72, 43: 73, 44: 15, 45: 88,
               46: 68, 47: 71, 48: 90, 49: 91}
_COMBO_TYPES = {0, 1, 2, 3, 5, 6}  # line, line with markers, columns, scatter and area combine in one chart
# XlTrendlineType <-> TrendKind
_TREND_IN = {-4132: 1, 5: 2, -4133: 3, 3: 4, 4: 5, 6: 6}
_TREND_OUT = {v: k for k, v in _TREND_IN.items()}
# XlErrorBarType <-> ErrorBarKind
_ERR_IN = {1: 1, 2: 2, -4155: 3, 4: 4, -4114: 5}
_INCLUDE_IN = {1: 1, 2: 2, 3: 3}  # XlErrorBarInclude: both, plus values, minus values
_ERR_OUT = {v: k for k, v in _ERR_IN.items()}


def _to_doubles(v):
    if v is None:
        return None
    if hasattr(v, "dtype") and hasattr(v, "tobytes"):  # numpy: send f64 bytes directly
        import numpy as np  # noqa: PLC0415

        return np.ascontiguousarray(v, dtype="<f8").tobytes()
    out = []
    for x in v:
        if x is None:
            out.append(float("nan"))
        else:
            try:
                out.append(float(x))
            except (TypeError, ValueError):
                from .types import datetime_to_serial

                out.append(datetime_to_serial(x))
    return out


# ---- chart elements (Chart.ChartArea, PlotArea, ChartTitle, Legend, Axes; Format pane) ----
# model::ChartElem
_ELEM_CHART_AREA, _ELEM_PLOT_AREA, _ELEM_TITLE, _ELEM_LEGEND, _ELEM_CAT_AXIS, _ELEM_VAL_AXIS, _ELEM_SERIES = 1, 2, 3, 4, 5, 6, 7
_ELEM_LEGEND_ENTRY, _ELEM_DATA_LABEL, _ELEM_MINOR_GRIDLINES = 22, 23, 24
_ELEM_VAL_AXIS2, _ELEM_POINT, _ELEM_CAT_TITLE, _ELEM_VAL_TITLE, _ELEM_GRIDLINES, _ELEM_LABELS, _ELEM_TREND, _ELEM_ERRBARS, _ELEM_VAL2_TITLE = (
    8, 9, 10, 11, 12, 13, 14, 15, 16)
# DataLabels.Position (XlDataLabelPosition) <-> the model's label position
_LABEL_POS_IN = {-4108: 0, 3: 1, 4: 2, 2: 3, -4131: 4, -4152: 5, 0: 6, 1: 7, 5: 8}
_LABEL_POS_OUT = {v: k for k, v in _LABEL_POS_IN.items()}
# LineFormat.DashStyle (MsoLineDashStyle) <-> the model's dash type
_DASH_IN = {1: 0, 3: 1, 2: 2, 4: 3, 5: 4, 7: 5}
_DASH_OUT = {v: k for k, v in _DASH_IN.items()}
# LineFormat.Style (MsoLineStyle) <-> the model's compound type
_COMPOUND_IN = {1: 0, 2: 1, 4: 2, 3: 3, 5: 4}
_COMPOUND_OUT = {v: k for k, v in _COMPOUND_IN.items()}
# FillFormat.Patterned (MsoPatternType) <-> the model's pattern
_PATTERN_IN = {1: 0, 2: 1, 3: 2, 4: 2, 5: 3, 6: 3, 7: 3, 19: 4, 20: 5, 21: 6, 22: 7, 13: 8, 14: 9, 23: 10, 25: 11, 18: 11, 24: 10}
_PATTERN_OUT = {0: 1, 1: 2, 2: 4, 3: 7, 4: 19, 5: 20, 6: 21, 7: 22, 8: 13, 9: 14, 10: 23, 11: 25}
# Axis.DisplayUnit (XlDisplayUnit) <-> the divisor
_UNIT_IN = {-4142: 1, -2: 100, -3: 1000, -4: 1e4, -5: 1e5, -6: 1e6, -7: 1e7, -8: 1e8, -9: 1e9, -10: 1e12}
_UNIT_OUT = {v: k for k, v in _UNIT_IN.items()}
_TICK_IN = {-4142: 0, 2: 1, 3: 2, 4: 3}
_TICK_OUT = {v: k for k, v in _TICK_IN.items()}
_TICKLABEL_IN = {4: 0, -4127: 1, -4134: 2, -4142: 3}
_TICKLABEL_OUT = {v: k for k, v in _TICKLABEL_IN.items()}
_LEGEND_TO_XL = {0: -4107, 1: -4152, 2: -4160, 3: -4131, 4: 2}
_LEGEND_IN = {v: k for k, v in _LEGEND_TO_XL.items()}
_MARKER_TO_XL = {-1: -4105, 0: -4142, 1: 8, 2: 1, 3: 2, 4: 3, 5: -4168, 6: 9}
_MARKER_IN = {v: k for k, v in _MARKER_TO_XL.items()}


def _select_elem(chart, elem, series=0, fmt=False, point=None):
    p = {"sheet": chart._sheet, "name": chart._name, "elem": elem, "series": series, "format": fmt}
    if point is not None:
        p["point"] = int(point)
    _rq("chart.select", **p)


class _ChartFont:
    """A chart text's font: Name, Size, Bold, Italic, Underline, Color (Excel's defaults when automatic: title 14 pt,
    the rest 9 pt). Works on the chart's elements and on a series' data labels."""

    def __init__(self, chart, key, default_size):
        self._c, self._k, self._def = chart, key, default_size

    @property
    def Size(self):  # noqa: N802
        return self._c._get().get(self._k + "_size") or self._def

    @Size.setter
    def Size(self, v):  # noqa: N802
        self._c._set(**{self._k + "_size": float(v)})

    @property
    def Bold(self):  # noqa: N802
        return bool(self._c._get().get(self._k + "_bold"))

    @Bold.setter
    def Bold(self, v):  # noqa: N802
        self._c._set(**{self._k + "_bold": bool(v)})

    @property
    def Italic(self):  # noqa: N802
        return bool(self._c._get().get(self._k + "_italic"))

    @Italic.setter
    def Italic(self, v):  # noqa: N802
        self._c._set(**{self._k + "_italic": bool(v)})

    @property
    def Underline(self):  # noqa: N802
        """xlUnderlineStyleSingle (2) or xlUnderlineStyleNone (-4142)."""
        return 2 if self._c._get().get(self._k + "_underline") else -4142

    @Underline.setter
    def Underline(self, v):  # noqa: N802
        self._c._set(**{self._k + "_underline": bool(v) and v != -4142})

    @property
    def FontStyle(self):  # noqa: N802
        b, i = self.Bold, self.Italic
        return "Bold Italic" if b and i else "Bold" if b else "Italic" if i else "Regular"

    @FontStyle.setter
    def FontStyle(self, v):  # noqa: N802
        v = str(v).lower()
        self._c._set(**{self._k + "_bold": "bold" in v, self._k + "_italic": "italic" in v})

    @property
    def Color(self):  # noqa: N802
        argb = self._c._get().get(self._k + "_color")
        return _from_argb(argb) if argb else 0x595959

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._c._set(**{self._k + "_color": _argb(v)})

    @property
    def Name(self):  # noqa: N802
        """The font's name ("Calibri" while it is the chart's automatic font)."""
        return self._c._get().get(self._k + "_name") or "Calibri"

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._c._set(**{self._k + "_name": str(v)})


class _ChartColor:
    """ForeColor of a chart fill or line (.RGB); 0 in the model = automatic."""

    def __init__(self, chart, key, auto_rgb):
        self._c, self._k, self._auto = chart, key, auto_rgb

    @property
    def RGB(self):  # noqa: N802
        argb = self._c._get()[self._k]
        return _from_argb(argb) if argb else self._auto

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        self._c._set(**{self._k: _argb(v)})


class _ChartFill:
    def __init__(self, chart, key, auto_rgb):
        self._c, self._k, self._auto = chart, key, auto_rgb

    @property
    def ForeColor(self):  # noqa: N802
        return _ChartColor(self._c, self._k, self._auto)

    @property
    def Visible(self):  # noqa: N802
        return -1

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        if not v:
            self._c._set(**{self._k: 0})

    def Solid(self):  # noqa: N802
        pass


class _ChartLine:
    def __init__(self, chart):
        self._c = chart

    @property
    def ForeColor(self):  # noqa: N802
        return _ChartColor(self._c, "chart_line", 0xD9D9D9)

    @property
    def Visible(self):  # noqa: N802
        return 0 if self._c._get()["chart_line_none"] else -1

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._c._set(chart_line_none=not v)


class _ChartAreaFormat:
    def __init__(self, chart, fill_key, auto_rgb, line):
        self._c, self._f, self._auto, self._line = chart, fill_key, auto_rgb, line

    @property
    def Fill(self):  # noqa: N802
        return _ChartFill(self._c, self._f, self._auto)

    @property
    def Line(self):  # noqa: N802
        if not self._line:
            return _ElemLine(self._c, self._f.replace("fill", "line"))
        return _ChartLine(self._c)


class _ElemLine:
    """The border of the plot area, the legend or the title (Format.Line): ForeColor.RGB, Visible."""

    def __init__(self, chart, key):
        self._c, self._k = chart, key

    @property
    def ForeColor(self):  # noqa: N802
        return _ChartColor(self._c, self._k, 0)

    @property
    def Visible(self):  # noqa: N802
        return -1 if self._c._get()[self._k] else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        if not v:
            self._c._set(**{self._k: 0})
        elif not self._c._get()[self._k]:
            self._c._set(**{self._k: _argb(0x404040)})


class _ChartInterior:
    def __init__(self, chart, key, auto_rgb):
        self._c, self._k, self._auto = chart, key, auto_rgb

    @property
    def Color(self):  # noqa: N802
        return _ChartColor(self._c, self._k, self._auto).RGB

    @Color.setter
    def Color(self, v):  # noqa: N802
        _ChartColor(self._c, self._k, self._auto).RGB = v

    @property
    def ColorIndex(self):  # noqa: N802
        return -4105 if not self._c._get()[self._k] else 0

    @ColorIndex.setter
    def ColorIndex(self, v):  # noqa: N802
        if int(v) in (-4105, -4142):
            self._c._set(**{self._k: 0})


class _ChartBorder:
    def __init__(self, chart):
        self._c = chart

    @property
    def Color(self):  # noqa: N802
        return _ChartLine(self._c).ForeColor.RGB

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._c._set(chart_line=_argb(v), chart_line_none=False)

    @property
    def LineStyle(self):  # noqa: N802
        return -4142 if self._c._get()["chart_line_none"] else 1

    @LineStyle.setter
    def LineStyle(self, v):  # noqa: N802
        self._c._set(chart_line_none=int(v) == -4142)


class ChartArea:
    def __init__(self, chart):
        self._c = chart

    @property
    def Format(self):  # noqa: N802
        return _ChartAreaFormat(self._c, "chart_fill", 0xFFFFFF, True)

    @property
    def Interior(self):  # noqa: N802
        return _ChartInterior(self._c, "chart_fill", 0xFFFFFF)

    @property
    def Border(self):  # noqa: N802
        return _ChartBorder(self._c)

    def Select(self):  # noqa: N802
        _select_elem(self._c, _ELEM_CHART_AREA)

    def ClearContents(self):  # noqa: N802
        """Remove the chart's data (its series and title); the chart stays."""
        for i in reversed(range(len(self._c._get()["series"]))):
            _rq("chart.series_delete", sheet=self._c._sheet, name=self._c._name, index=i)
        self._c._set(has_title=False)

    def Clear(self):  # noqa: N802
        self.ClearContents()
        self.ClearFormats()

    def ClearFormats(self):  # noqa: N802
        self._c._set(chart_fill=0, chart_line=0, chart_line_none=False, plot_fill=0, title_size=0.0, title_color=0, title_bold=False,
                     legend_size=0.0, legend_color=0, legend_bold=False, axis_size=0.0, axis_color=0, axis_bold=False)


def _manual_prop(key, k):
    """Left / Top / Width / Height (points, from the chart area's corner) of an element placed by hand (its manual
    layout, kept as fractions of the chart); 0 while it is placed automatically. Setting one places it by hand."""

    def g(self):
        d = self._c._get()
        m = d.get(key) or [False, 0, 0, 0, 0]
        return float(m[k]) * float(d["width" if k in (1, 3) else "height"]) if m[0] else 0.0

    def s(self, v):
        d = self._c._get()
        m = list(d.get(key) or [False, 0, 0, 0, 0])
        if not m[0]:
            m = [True, 0.05, 0.05, 0.25, 0.2]
        m[0] = True
        size = float(d["width" if k in (1, 3) else "height"]) or 1.0
        m[k] = max(0.0, min(1.0, float(v) / size))
        self._c._set(**{key: m})

    return property(g, s)


class PlotArea:
    def __init__(self, chart):
        self._c = chart

    @property
    def Format(self):  # noqa: N802
        return _ChartAreaFormat(self._c, "plot_fill", 0xFFFFFF, False)

    @property
    def Interior(self):  # noqa: N802
        return _ChartInterior(self._c, "plot_fill", 0xFFFFFF)

    def Select(self):  # noqa: N802
        _select_elem(self._c, _ELEM_PLOT_AREA)

    def ClearFormats(self):  # noqa: N802
        self._c._set(plot_fill=0)

    Left = InsideLeft = _manual_prop("plot_rect", 1)
    Top = InsideTop = _manual_prop("plot_rect", 2)
    Width = InsideWidth = _manual_prop("plot_rect", 3)
    Height = InsideHeight = _manual_prop("plot_rect", 4)


class Legend:
    def __init__(self, chart):
        self._c = chart

    @property
    def Position(self):  # noqa: N802
        return _LEGEND_TO_XL.get(self._c._get()["legend_pos"], -4107)

    @Position.setter
    def Position(self, v):  # noqa: N802
        if int(v) not in _LEGEND_IN:
            raise UnsupportedError(f"Unsupported legend position: {v}")
        self._c._set(legend_pos=_LEGEND_IN[int(v)], has_legend=True)

    @property
    def Font(self):  # noqa: N802
        return _ChartFont(self._c, "legend", 9)

    def Select(self):  # noqa: N802
        _select_elem(self._c, _ELEM_LEGEND)

    def Delete(self):  # noqa: N802
        self._c._set(has_legend=False)

    @property
    def IncludeInLayout(self):  # noqa: N802
        """False: the legend overlays the plot area ("Show the legend without overlapping the chart" off)."""
        return not self._c._get().get("legend_overlay", False)

    @IncludeInLayout.setter
    def IncludeInLayout(self, v):  # noqa: N802
        self._c._set(legend_overlay=not v)

    Left = _manual_prop("legend_rect", 1)
    Top = _manual_prop("legend_rect", 2)
    Width = _manual_prop("legend_rect", 3)
    Height = _manual_prop("legend_rect", 4)

    @property
    def Format(self):  # noqa: N802
        return _ChartAreaFormat(self._c, "legend_fill", 0xFFFFFF, False)

    def LegendEntries(self, Index=None):  # noqa: N802,N803
        """The legend's entries, one per series shown (LegendEntries(i), 1-based; .Count)."""
        entries = _LegendEntries(self._c)
        return entries if Index is None else entries(Index)


_FONT_KEYS = ("size", "color", "bold", "name", "italic", "underline")


class _OwnFont:
    """A font that falls back on another until it is set: one legend entry's (else the legend's), one data label's
    (else its series' labels'). Serves _ChartFont as its chart (_get / _set with the "f" prefix)."""

    def __init__(self, read, write):
        self._read, self._write = read, write  # read() -> {key: value} of the font in effect; write(dict) sets all of it

    def _get(self):
        return {"f_" + k: v for k, v in self._read().items()}

    def _set(self, **kw):
        font = dict(self._read())
        for k, v in kw.items():
            font[k[2:]] = v
        self._write(font)


def _font_of(d, prefix):
    return {k: d.get(f"{prefix}_{k}") for k in _FONT_KEYS}


class _LegendEntries:
    def __init__(self, chart):
        self._c = chart

    def _cat_series(self):
        """A pie's (or a one-series vary-colors chart's) legend lists its categories: that series, else None."""
        d = self._c._get()
        ss = d["series"]
        shown = [i for i, se in enumerate(ss) if not se.get("filtered")]
        if not shown:
            return None
        if d.get("type") in (7, 8, 26):
            return shown[0]
        se = ss[shown[0]]
        if len(shown) == 1 and se.get("vary_points") and d.get("type") in (2, 3, 4, 31, 32, 33):
            return shown[0]
        return None

    def _shown(self):
        cs = self._cat_series()
        if cs is not None:
            n = Series(self._c, cs).Points().Count
            hidden = {q.get("index") for q in self._c._get()["series"][cs].get("points", []) if q.get("legend_hidden")}
            return [(cs, k) for k in range(n) if k not in hidden]
        return [(i, None) for i, se in enumerate(self._c._get()["series"]) if not se.get("filtered") and not se.get("legend_hidden")]

    @property
    def Count(self):  # noqa: N802
        return len(self._shown())

    def __len__(self):
        return self.Count

    def __call__(self, Index):  # noqa: N803
        shown = self._shown()
        if not 1 <= int(Index) <= len(shown):
            raise SubscriptOutOfRangeError(f"Legend entry {Index} does not exist")
        series, cat = shown[int(Index) - 1]
        return LegendEntry(self._c, int(Index) - 1, series, cat)

    Item = __call__

    def __iter__(self):
        return (self(i) for i in range(1, self.Count + 1))


class LegendEntry:
    """Legend.LegendEntries(i): one entry of the legend (a series, or a pie's category): its own Font (Format Legend
    Entry), Select(), Delete() (a series' entry leaves the legend, the series stays)."""

    def __init__(self, chart, index, series, cat=None):
        self._c, self._k, self._series, self._cat = chart, index, series, cat  # 0-based entry, its series, its category

    def _read(self):
        d = self._c._get()
        se = d["series"][self._series]
        if self._cat is None:
            if se.get("legend_entry_font_set"):
                return _font_of(se, "legend_entry_font")
        else:
            for q in se.get("points", []):
                if q.get("index") == self._cat and q.get("legend_font_set"):
                    return _font_of(q, "legend_font")
        return _font_of(d, "legend")

    def _write(self, font):
        if self._cat is None:
            props = {f"legend_entry_font_{k}": v for k, v in font.items() if v is not None}
        else:
            props = {"point_set": dict({"index": self._cat}, **{f"legend_font_{k}": v for k, v in font.items() if v is not None})}
        _rq("chart.series_set", sheet=self._c._sheet, name=self._c._name, index=self._series, props=props)

    @property
    def Font(self):  # noqa: N802
        return _ChartFont(_OwnFont(self._read, self._write), "f", 9)

    def Select(self):  # noqa: N802
        _select_elem(self._c, _ELEM_LEGEND_ENTRY, self._series if self._cat is None else -1, point=self._k if self._cat is None else self._cat)

    def Delete(self):  # noqa: N802
        props = {"legend_hidden": True} if self._cat is None else {"point_set": {"index": self._cat, "legend_hidden": True}}
        _rq("chart.series_set", sheet=self._c._sheet, name=self._c._name, index=self._series, props=props)


class _AxisTitle:
    def __init__(self, axis):
        self._a = axis

    @property
    def Text(self):  # noqa: N802
        """The title as the chart shows it (a title linked to a cell: that cell's text)."""
        d = self._a._c._get()
        return d.get(self._a._p + "_title_text", d[self._a._p + "_title"])

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._a._c._set(**{self._a._p + "_title": str(v), self._a._p + "_has_title": True})

    Caption = Text

    @property
    def Formula(self):  # noqa: N802
        """="Sheet1!$B$1" when the title is linked to a cell, else its text."""
        return self._a._c._get()[self._a._p + "_title"]

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        v = str(v)
        if v and not v.startswith("="):
            v = "=" + v
        self._a._c._set(**{self._a._p + "_title": v, self._a._p + "_has_title": True})

    def Select(self):  # noqa: N802
        _select_elem(self._a._c, {"cat": _ELEM_CAT_TITLE, "val": _ELEM_VAL_TITLE}.get(self._a._p, _ELEM_VAL2_TITLE))

    @property
    def _c(self):
        return self._a._c

    @property
    def Left(self):  # noqa: N802
        """Points from the chart area's left edge when the title was placed by hand (0: placed automatically)."""
        return _manual_prop(self._a._p + "_title_rect", 1).fget(self)

    @Left.setter
    def Left(self, v):  # noqa: N802
        _manual_prop(self._a._p + "_title_rect", 1).fset(self, v)

    @property
    def Top(self):  # noqa: N802
        return _manual_prop(self._a._p + "_title_rect", 2).fget(self)

    @Top.setter
    def Top(self, v):  # noqa: N802
        _manual_prop(self._a._p + "_title_rect", 2).fset(self, v)

    @property
    def Font(self):  # noqa: N802
        return _ChartFont(self._a._c, self._a._p + "_tfont", 10)


class _TickLabels:
    def __init__(self, axis):
        self._a = axis

    @property
    def NumberFormat(self):  # noqa: N802
        return self._a._c._get()[self._a._p + "_format"] or "General"

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        v = str(v)
        self._a._c._set(**{self._a._p + "_format": "" if v.lower() == "general" else v})

    @property
    def NumberFormatLinked(self):  # noqa: N802
        """True: the labels take the source cells' number format (Format Axis > Number > Linked to source)."""
        return not self._a._c._get()[self._a._p + "_format"]

    @NumberFormatLinked.setter
    def NumberFormatLinked(self, v):  # noqa: N802
        if v:
            self._a._c._set(**{self._a._p + "_format": ""})
        elif not self._a._c._get()[self._a._p + "_format"]:
            self._a._c._set(**{self._a._p + "_format": "General"})

    @property
    def Font(self):  # noqa: N802
        """This axis' own font (Format Axis > Text Options)."""
        return _ChartFont(self._a._c, self._a._p + "_font", 9)

    @property
    def Offset(self):  # noqa: N802
        """Label distance from axis (0..1000, 100 by default)."""
        return int(self._a._c._get().get(self._a._p + "_label_offset", 100))

    @Offset.setter
    def Offset(self, v):  # noqa: N802
        self._a._c._set(**{self._a._p + "_label_offset": int(v)})

    @property
    def MultiLevel(self):  # noqa: N802
        """Multi-level Category Labels: several category columns (a PivotChart's row fields) in rows of grouped labels."""
        return bool(self._a._c._get().get(self._a._p + "_multi_level", True))

    @MultiLevel.setter
    def MultiLevel(self, v):  # noqa: N802
        self._a._c._set(**{self._a._p + "_multi_level": bool(v)})


class _ChartTitle:
    def __init__(self, chart: "Chart"):
        self._c = chart

    @property
    def Font(self):  # noqa: N802
        return _ChartFont(self._c, "title", 14)

    def Select(self):  # noqa: N802
        _select_elem(self._c, _ELEM_TITLE)

    def Delete(self):  # noqa: N802
        self._c._set(has_title=False)

    @property
    def Text(self):  # noqa: N802
        """The title as the chart shows it: a linked cell's text, or "Chart Title" while it has none."""
        d = self._c._get()
        return d.get("title_text", d["title"])

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._c._set(title=str(v), has_title=True)

    Caption = Text

    @property
    def Formula(self):  # noqa: N802
        """"=Sheet1!$B$1" when the title is linked to a cell (it follows the cell), else its text."""
        return self._c._get()["title"]

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        self._c._set(title=str(v), has_title=True)  # "=Sheet1!$B$1" links the title to a cell; text is the title

    @property
    def IncludeInLayout(self):  # noqa: N802
        """False: Centered Overlay (the plot area keeps its size under the title)."""
        return not self._c._get().get("title_overlay", False)

    @IncludeInLayout.setter
    def IncludeInLayout(self, v):  # noqa: N802
        self._c._set(title_overlay=not v)

    Left = _manual_prop("title_rect", 1)
    Top = _manual_prop("title_rect", 2)

    @property
    def Format(self):  # noqa: N802
        return _ChartAreaFormat(self._c, "title_fill", 0xFFFFFF, False)


class _Gridlines:
    """Axis.MajorGridlines / MinorGridlines: Select() (Format Major / Minor Gridlines), Delete()."""

    def __init__(self, axis, minor=False):
        self._a, self._minor = axis, minor

    def Select(self):  # noqa: N802
        _select_elem(self._a._c, _ELEM_MINOR_GRIDLINES if self._minor else _ELEM_GRIDLINES)

    def Delete(self):  # noqa: N802
        if self._minor:
            self._a.HasMinorGridlines = False
        else:
            self._a.HasMajorGridlines = False


class _AxisLine:
    """Axis.Format.Line: the axis line's color, whether it is drawn, its width (points)."""

    def __init__(self, axis):
        self._a = axis

    @property
    def ForeColor(self):  # noqa: N802
        return _ChartColor(self._a._c, self._a._p + "_line_color", 0xBFBFBF)

    @property
    def Visible(self):  # noqa: N802
        return 0 if self._a._get("no_line", False) else -1

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        props = {self._a._p + "_no_line": not v}
        if v and not self._a._get("line_color", 0):
            props[self._a._p + "_line_color"] = _argb(0xBFBFBF)
        self._a._c._set(**props)

    @property
    def Weight(self):  # noqa: N802
        return self._a._get("line_weight", 0) or 0.75

    @Weight.setter
    def Weight(self, v):  # noqa: N802
        self._a._set1("line_weight", float(v))


class _AxisFormat:
    def __init__(self, axis):
        self._a = axis

    @property
    def Line(self):  # noqa: N802
        return _AxisLine(self._a)


class _DisplayUnitLabel:
    """Axis.DisplayUnitLabel: the label of the axis' display unit."""

    _NAMES = {100.0: "Hundreds", 1000.0: "Thousands", 1e6: "Millions", 1e9: "Billions", 1e12: "Trillions"}

    def __init__(self, axis):
        self._a = axis

    @property
    def Text(self):  # noqa: N802
        u = float(self._a._get("display_unit", 1.0))
        if u in self._NAMES:
            return self._NAMES[u]
        if u in (1e4, 1e5, 1e7, 1e8):
            return str(int(u))
        return f"x {u:g}"

    Caption = Text

    @property
    def Visible(self):  # noqa: N802
        return self._a.HasDisplayUnitLabel

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._a.HasDisplayUnitLabel = bool(v)


class _Axis:
    """Chart.Axes(Type, AxisGroup): the category axis (xlCategory), the value axis (xlValue) or the secondary value
    axis (xlValue, xlSecondary)."""

    def __init__(self, chart: "Chart", kind: int, group: int = 1):
        self._c, self._k, self._g = chart, kind, group

    # value axis (2): "val_" keys, the secondary one "val2_"; category axis (1): "cat_"
    @property
    def _p(self):
        if self._k == 2:
            return "val2" if self._g == 2 else "val"
        return "cat"

    def _get(self, key, default=None):
        return self._c._get().get(self._p + "_" + key, default)

    def _set1(self, key, v):
        self._c._set(**{self._p + "_" + key: v})

    # the category axis' type: text or dates (Format Axis > Axis Type), and a date axis' base unit
    _CAT_TYPE_OUT = {0: -4105, 1: 2, 2: 3}
    _CAT_TYPE_IN = {-4105: 0, 2: 1, 3: 2}

    @property
    def CategoryType(self):  # noqa: N802
        """xlAutomaticScale (dates make a date axis), xlCategoryScale (text axis) or xlTimeScale (date axis)."""
        return self._CAT_TYPE_OUT.get(int(self._c._get().get("cat_type", 0)), -4105)

    @CategoryType.setter
    def CategoryType(self, v):  # noqa: N802
        if int(v) not in self._CAT_TYPE_IN:
            raise UnsupportedError(f"Unsupported category type: {v}")
        self._c._set(cat_type=self._CAT_TYPE_IN[int(v)])

    @property
    def BaseUnit(self):  # noqa: N802
        """A date axis' base unit: xlDays, xlMonths or xlYears (automatic: from the dates' spacing)."""
        b = int(self._c._get().get("base_unit", -1))
        return 0 if b < 0 else b

    @BaseUnit.setter
    def BaseUnit(self, v):  # noqa: N802
        self._c._set(base_unit=int(v))

    @property
    def BaseUnitIsAuto(self):  # noqa: N802
        return int(self._c._get().get("base_unit", -1)) < 0

    @BaseUnitIsAuto.setter
    def BaseUnitIsAuto(self, v):  # noqa: N802
        if v:
            self._c._set(base_unit=-1)

    @property
    def MajorUnitScale(self):  # noqa: N802
        """A date axis' major unit counted in xlDays, xlMonths or xlYears (MajorUnit is how many)."""
        m = int(self._c._get().get("major_unit_scale", -1))
        return self.BaseUnit if m < 0 else m

    @MajorUnitScale.setter
    def MajorUnitScale(self, v):  # noqa: N802
        self._c._set(major_unit_scale=int(v))

    # Bounds: each end automatic or fixed (setting a bound fixes it)
    @property
    def MinimumScale(self):  # noqa: N802
        return self._get("min", 0.0)

    @MinimumScale.setter
    def MinimumScale(self, v):  # noqa: N802
        self._set1("min", float(v))

    @property
    def MaximumScale(self):  # noqa: N802
        return self._get("max", 1.0)

    @MaximumScale.setter
    def MaximumScale(self, v):  # noqa: N802
        self._set1("max", float(v))

    @property
    def MinimumScaleIsAuto(self):  # noqa: N802
        return bool(self._get("min_auto", True))

    @MinimumScaleIsAuto.setter
    def MinimumScaleIsAuto(self, v):  # noqa: N802
        self._set1("min_auto", bool(v))

    @property
    def MaximumScaleIsAuto(self):  # noqa: N802
        return bool(self._get("max_auto", True))

    @MaximumScaleIsAuto.setter
    def MaximumScaleIsAuto(self, v):  # noqa: N802
        self._set1("max_auto", bool(v))

    # Units (0 in the model: automatic)
    @property
    def MajorUnit(self):  # noqa: N802
        return self._get("major", 0.0)

    @MajorUnit.setter
    def MajorUnit(self, v):  # noqa: N802
        self._set1("major", float(v))

    @property
    def MajorUnitIsAuto(self):  # noqa: N802
        return not self._get("major", 0.0)

    @MajorUnitIsAuto.setter
    def MajorUnitIsAuto(self, v):  # noqa: N802
        if v:
            self._set1("major", 0.0)

    @property
    def MinorUnit(self):  # noqa: N802
        return self._get("minor", 0.0)

    @MinorUnit.setter
    def MinorUnit(self, v):  # noqa: N802
        self._set1("minor", float(v))

    @property
    def MinorUnitIsAuto(self):  # noqa: N802
        return not self._get("minor", 0.0)

    @MinorUnitIsAuto.setter
    def MinorUnitIsAuto(self, v):  # noqa: N802
        if v:
            self._set1("minor", 0.0)

    @property
    def ScaleType(self):  # noqa: N802
        """xlScaleLinear (-4132) or xlScaleLogarithmic (-4133)."""
        return -4133 if self._get("log", False) else -4132

    @ScaleType.setter
    def ScaleType(self, v):  # noqa: N802
        self._set1("log", int(v) == -4133)

    @property
    def LogBase(self):  # noqa: N802
        return self._get("log_base", 10.0)

    @LogBase.setter
    def LogBase(self, v):  # noqa: N802
        self._set1("log_base", float(v))

    @property
    def ReversePlotOrder(self):  # noqa: N802
        return bool(self._get("reverse", False))

    @ReversePlotOrder.setter
    def ReversePlotOrder(self, v):  # noqa: N802
        self._set1("reverse", bool(v))

    @property
    def Crosses(self):  # noqa: N802
        """Where the other axis crosses: xlAxisCrossesAutomatic, xlAxisCrossesCustom (CrossesAt), xlAxisCrossesMaximum."""
        return {0: -4105, 1: -4114, 2: 2}.get(self._get("crosses", 0), -4105)

    @Crosses.setter
    def Crosses(self, v):  # noqa: N802
        m = {-4105: 0, -4114: 1, 2: 2, 4: 0}
        if int(v) not in m:
            raise UnsupportedError(f"Unsupported Crosses value: {v}")
        self._set1("crosses", m[int(v)])

    @property
    def CrossesAt(self):  # noqa: N802
        return self._get("crosses_at", 0.0)

    @CrossesAt.setter
    def CrossesAt(self, v):  # noqa: N802
        self._c._set(**{self._p + "_crosses_at": float(v), self._p + "_crosses": 1})

    @property
    def DisplayUnit(self):  # noqa: N802
        """xlNone, xlHundreds, xlThousands... xlMillionMillions: the tick labels divided."""
        u = self._get("display_unit", 1.0)
        # a unit of its own (DisplayUnitCustom) reads xlCustom (-4114), as Excel 16's (measured)
        return _UNIT_OUT.get(u, -4142 if u == 1 else -4114)

    @DisplayUnit.setter
    def DisplayUnit(self, v):  # noqa: N802
        if int(v) == -4114:  # xlCustom: DisplayUnitCustom gives the number
            return
        if int(v) not in _UNIT_IN:
            raise UnsupportedError(f"Unsupported display unit: {v}")
        self._set1("display_unit", float(_UNIT_IN[int(v)]))

    @property
    def AxisGroup(self):  # noqa: N802
        """xlPrimary (1) or xlSecondary (2): which value axis this is (a price on one, a volume on the other)."""
        return self._g if self._k == 2 else 1

    @property
    def DisplayUnitCustom(self):  # noqa: N802
        """The number the tick labels are divided by (1 without a display unit); setting it makes DisplayUnit
        xlCustom (-4114)."""
        return float(self._get("display_unit", 1.0))

    @DisplayUnitCustom.setter
    def DisplayUnitCustom(self, v):  # noqa: N802
        if not float(v) > 0:
            raise ValueError("DisplayUnitCustom must be greater than 0")
        self._set1("display_unit", float(v))

    @property
    def HasDisplayUnitLabel(self):  # noqa: N802
        """The display unit's label ("Thousands", "x 250") above the axis; True from the moment a unit is chosen."""
        return bool(self._get("display_unit", 1.0) != 1 and self._get("unit_label", True))

    @HasDisplayUnitLabel.setter
    def HasDisplayUnitLabel(self, v):  # noqa: N802
        self._set1("unit_label", bool(v))

    @property
    def DisplayUnitLabel(self):  # noqa: N802
        """The display unit's label: Text / Caption (as the chart shows it)."""
        return _DisplayUnitLabel(self)

    @property
    def TickMarkSpacing(self):  # noqa: N802
        """The category axis: a tick mark every this many categories (with the labels until set)."""
        if self._k != 1:
            raise ApplicationDefinedError("TickMarkSpacing is the category axis'")
        return int(self._get("mark_interval", 0) or self._get("label_interval", 0) or 1)

    @TickMarkSpacing.setter
    def TickMarkSpacing(self, v):  # noqa: N802
        if self._k != 1:
            raise ApplicationDefinedError("TickMarkSpacing is the category axis'")
        if not 1 <= int(v) <= 31999:
            raise ValueError("TickMarkSpacing must be from 1 to 31999")
        self._set1("mark_interval", int(v))

    @property
    def CategoryNames(self):  # noqa: N802
        """The category axis' labels, as the chart shows them; setting a list sets every series' categories."""
        sc = self._c.SeriesCollection()
        if not len(sc):
            return []
        xs = sc(1).XValues
        return list(xs) if xs is not None else []

    @CategoryNames.setter
    def CategoryNames(self, v):  # noqa: N802
        names = list(v) if not isinstance(v, str) else [v]
        for sr in self._c.SeriesCollection():
            sr.XValues = names

    @property
    def MajorTickMark(self):  # noqa: N802
        t = self._get("major_tick", -1)
        return _TICK_OUT.get(t, 3)

    @MajorTickMark.setter
    def MajorTickMark(self, v):  # noqa: N802
        self._set1("major_tick", _TICK_IN.get(int(v), 2))

    @property
    def MinorTickMark(self):  # noqa: N802
        return _TICK_OUT.get(self._get("minor_tick", 0), -4142)

    @MinorTickMark.setter
    def MinorTickMark(self, v):  # noqa: N802
        self._set1("minor_tick", _TICK_IN.get(int(v), 0))

    @property
    def TickLabelPosition(self):  # noqa: N802
        return _TICKLABEL_OUT.get(self._get("label_pos", 0), 4)

    @TickLabelPosition.setter
    def TickLabelPosition(self, v):  # noqa: N802
        if int(v) not in _TICKLABEL_IN:
            raise UnsupportedError(f"Unsupported tick label position: {v}")
        self._set1("label_pos", _TICKLABEL_IN[int(v)])

    @property
    def AxisBetweenCategories(self):  # noqa: N802
        return bool(self._get("between", True))

    @AxisBetweenCategories.setter
    def AxisBetweenCategories(self, v):  # noqa: N802
        self._set1("between", bool(v))

    @property
    def TickLabelSpacing(self):  # noqa: N802
        return self._get("label_interval", 0) or 1

    @TickLabelSpacing.setter
    def TickLabelSpacing(self, v):  # noqa: N802
        self._set1("label_interval", int(v))

    @property
    def TickLabelSpacingIsAuto(self):  # noqa: N802
        return not self._get("label_interval", 0)

    @TickLabelSpacingIsAuto.setter
    def TickLabelSpacingIsAuto(self, v):  # noqa: N802
        if v:
            self._set1("label_interval", 0)

    @property
    def HasTitle(self):  # noqa: N802
        return bool(self._c._get()[self._p + "_has_title"])

    @HasTitle.setter
    def HasTitle(self, v):  # noqa: N802
        props = {self._p + "_has_title": bool(v)}
        if v and not self._c._get()[self._p + "_title"]:
            props[self._p + "_title"] = "Axis Title"
        self._c._set(**props)

    @property
    def AxisTitle(self):  # noqa: N802
        return _AxisTitle(self)

    @property
    def HasMajorGridlines(self):  # noqa: N802
        return bool(self._c._get()[self._p + "_grid"])

    @HasMajorGridlines.setter
    def HasMajorGridlines(self, v):  # noqa: N802
        self._c._set(**{self._p + "_grid": bool(v)})

    @property
    def HasMinorGridlines(self):  # noqa: N802
        return bool(self._get("minor_grid", False))

    @HasMinorGridlines.setter
    def HasMinorGridlines(self, v):  # noqa: N802
        self._set1("minor_grid", bool(v))

    @property
    def MajorGridlines(self):  # noqa: N802
        return _Gridlines(self)

    @property
    def MinorGridlines(self):  # noqa: N802
        return _Gridlines(self, minor=True)

    @property
    def TickLabels(self):  # noqa: N802
        return _TickLabels(self)

    @property
    def Format(self):  # noqa: N802
        """Format Axis > Fill & Line: .Line (ForeColor.RGB, Visible, Weight)."""
        return _AxisFormat(self)

    @property
    def Visible(self):  # noqa: N802
        """[YC] Whether the axis shows its labels and tick marks (Chart.HasAxis)."""
        return bool(self._c._get()[self._p + "_visible"])

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._c._set(**{self._p + "_visible": bool(v)})

    def Delete(self):  # noqa: N802
        self.Visible = False

    def Select(self):  # noqa: N802
        _select_elem(self._c, {"val": _ELEM_VAL_AXIS, "val2": _ELEM_VAL_AXIS2}.get(self._p, _ELEM_CAT_AXIS))


class Series:
    def __init__(self, chart: "Chart", index: int):
        self._c, self._i = chart, index  # 0-based

    def __repr__(self):
        return f"<Series {self._i + 1} of {self._c._name}>"

    def _get(self) -> dict:
        ss = self._c._get()["series"]
        if self._i >= len(ss):
            raise SubscriptOutOfRangeError("The series no longer exists")
        return ss[self._i]

    def _set(self, **props):
        _rq("chart.series_set", sheet=self._c._sheet, name=self._c._name, index=self._i, props=props)

    @property
    def Name(self):  # noqa: N802
        n = self._get()["name"]
        if n.startswith("="):
            from . import excel

            try:
                return str(excel.Range(n[1:]).Text)
            except Exception:  # noqa: BLE001
                return n
        return n

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._set(name=str(v))

    @property
    def Values(self):  # noqa: N802
        """The series' values, as Excel gives them (a tuple; dates as serial numbers). Its range is in Formula."""
        return _series_values(self._get()["values"])

    @Values.setter
    def Values(self, v):  # noqa: N802
        if isinstance(v, str) or hasattr(v, "Address"):
            self._set(values=_addr(v))
        else:
            self.SetData(y=v)

    @property
    def XValues(self):  # noqa: N802
        """The category (X) values (a tuple; empty when the chart numbers the points 1, 2, 3...)."""
        return _series_values(self._get()["x_values"])

    @XValues.setter
    def XValues(self, v):  # noqa: N802
        if isinstance(v, str) or hasattr(v, "Address"):
            self._set(x_values=_addr(v))
        else:
            raise UnsupportedError("To set XValues from an array, use Series.SetData(x=..., y=...)")

    @property
    def AxisGroup(self):  # noqa: N802
        return self._get()["axis_group"] + 1

    @AxisGroup.setter
    def AxisGroup(self, v):  # noqa: N802
        self._set(axis_group=1 if int(v) == 2 else 0)

    @property
    def Formula(self):  # noqa: N802
        """=SERIES(name, X values, values, order), as Excel writes it ("" for Python data)."""
        return self._get().get("formula", "")

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        f = str(v)
        if not f.lstrip("= ").upper().startswith("SERIES("):
            raise ValueError("Series.Formula is =SERIES(name, X values, values, order)")
        before = self._get().get("formula", "")
        self._set(formula=f)
        if self._get().get("formula", "") == before and f.replace(" ", "") != before.replace(" ", ""):
            raise ValueError(f"Not a SERIES formula with ranges of cells: {f}")

    @property
    def IsFiltered(self):  # noqa: N802
        """Chart Filters: the series is left out of the chart (it keeps its place and formatting)."""
        return bool(self._get().get("filtered", False))

    @IsFiltered.setter
    def IsFiltered(self, v):  # noqa: N802
        self._set(filtered=bool(v))

    @property
    def Color(self):  # noqa: N802
        """[YC] Series color (RGB()); 0 = automatic."""
        return self._get()["color"]

    @Color.setter
    def Color(self, v):  # noqa: N802
        self._set(color=_excel_color(v))

    @property
    def ChartType(self):  # noqa: N802
        """The series' own chart type (combo charts: lines, columns, area and scatter in one chart)."""
        t = self._get().get("type", -1)
        return _TYPE_TO_XL.get(t if t >= 0 else self._c._get()["type"], 4)

    @ChartType.setter
    def ChartType(self, v):  # noqa: N802
        t = _XL_TO_TYPE.get(int(v))
        if t is None:
            raise UnsupportedError(f"Unsupported chart type: {v}")
        if t not in _COMBO_TYPES or self._c._get()["type"] not in _COMBO_TYPES:
            raise UnsupportedError("Only line, column, area and XY scatter series combine in one chart")
        self._set(type=t)

    @property
    def HasDataLabels(self):  # noqa: N802
        return bool(self._get().get("data_labels"))

    @HasDataLabels.setter
    def HasDataLabels(self, v):  # noqa: N802
        self._set(data_labels=bool(v))

    def ApplyDataLabels(self, Type=None, LegendKey=None, AutoText=None, HasLeaderLines=None, ShowSeriesName=None,  # noqa: N802,N803
                        ShowCategoryName=None, ShowValue=True, ShowPercentage=None, ShowBubbleSize=None, Separator=None):
        """Data labels with what they show: the value, the series' and category's names, the percentage (pies)
        (Type xlDataLabelsShowNone removes them)."""
        if Type is not None and int(Type) == -4142:
            self._set(data_labels=False)
            return
        show = (4 if ShowValue else 0) | (1 if ShowSeriesName else 0) | (2 if ShowCategoryName else 0) | (8 if ShowPercentage else 0)
        if Type is not None:
            show = {2: 4, 3: 8, 4: 2, 5: 10, 6: 8}.get(int(Type), show)  # xlDataLabelsShowValue, Percent, Label, LabelAndPercent
        props = {"data_labels": bool(show)}
        if show not in (0, 4):  # (the value alone is what a series' labels show already)
            props["label_show"] = show
        if Separator is not None:
            props["label_sep"] = str(Separator)
        self._set(**props)

    @property
    def DataLabels(self):  # noqa: N802
        """Series.DataLabels.ShowValue = True as a recorded macro writes it; DataLabels() and DataLabels(i) too."""
        return _DataLabels(self)

    @property
    def HasLeaderLines(self):  # noqa: N802
        return bool(self._get().get("leader_lines", True))

    @HasLeaderLines.setter
    def HasLeaderLines(self, v):  # noqa: N802
        self._set(leader_lines=bool(v))

    @property
    def Trendlines(self):  # noqa: N802
        """The series' trendlines: Trendlines.Add adds one more (Trendlines().Add too); Trendlines(2) is the second."""
        return _Trendlines(self)

    @property
    def HasErrorBars(self):  # noqa: N802
        return self._get().get("err", 0) != 0

    @HasErrorBars.setter
    def HasErrorBars(self, v):  # noqa: N802
        if not v:
            self._set(err=0)
        elif not self.HasErrorBars:
            self._set(err=1, err_amount=1.0)

    @property
    def ErrorBars(self):  # noqa: N802
        return _ErrorBars(self)

    @property
    def XErrorBars(self):  # noqa: N802
        """[YC] A scatter chart's X error bars (ErrorBar(Direction=xlX) adds them)."""
        return _ErrorBars(self, True)

    def ErrorBar(self, Direction=1, Include=1, Type=1, Amount=None, MinusValues=None):  # noqa: N802,N803
        """Y error bars (Direction xlY) or a scatter chart's X error bars (xlX): fixed value, percentage, standard
        deviation(s), standard error, or custom amounts (xlErrorBarTypeCustom: Amount and MinusValues are ranges, lists
        or numbers); Include picks the directions."""
        x = int(Direction) == -4168
        if not x and int(Direction) != 1:
            raise UnsupportedError(f"Unsupported error bar direction: {Direction}")
        pre = "x" if x else ""
        if int(Include) == -4142:
            self._set(**{pre + "err": 0})
            return _ErrorBars(self, x)
        if x:
            k = _ERR_IN.get(int(Type))
            if k is None:
                raise UnsupportedError(f"Unsupported error bar type: {Type}")
            p = {"xerr": k, "xerr_include": _INCLUDE_IN.get(int(Include), 1)}
            if k == 5:
                def amounts(v):
                    if v is None:
                        return ""
                    if isinstance(v, (list, tuple)):
                        return "{" + ",".join(repr(float(q)) for q in v) + "}"
                    if isinstance(v, (int, float)):
                        return repr(float(v))
                    return _addr(v)

                p["xerr_plus"] = amounts(Amount)
                p["xerr_minus"] = amounts(MinusValues) if MinusValues is not None else p["xerr_plus"]
            else:
                p["xerr_amount"] = (5.0 if k == 2 else 1.0) if Amount is None else float(Amount)
            self._set(**p)
            return _ErrorBars(self, True)
        k = _ERR_IN.get(int(Type))
        if k is None:
            raise UnsupportedError(f"Unsupported error bar type: {Type}")
        inc = _INCLUDE_IN.get(int(Include), 1)
        if k == 5:
            def amounts(v):
                if v is None:
                    return ""
                if isinstance(v, (list, tuple)):
                    return "{" + ",".join(repr(float(x)) for x in v) + "}"
                if isinstance(v, (int, float)):
                    return repr(float(v))
                return _addr(v)

            plus = amounts(Amount)
            self._set(err=5, err_plus=plus, err_minus=amounts(MinusValues) if MinusValues is not None else plus, err_include=inc)
            return _ErrorBars(self)
        amount = 1.0 if Amount is None else float(Amount)
        if Amount is None and k == 2:
            amount = 5.0
        self._set(err=k, err_amount=amount, err_include=inc)
        return _ErrorBars(self)

    def Points(self, Index=None):  # noqa: N802,N803
        pts = _Points(self)
        return pts if Index is None else pts.Item(Index)

    @property
    def Count(self):  # noqa: N802
        """[YC] Number of points of the Python data."""
        return self._get()["count"]

    def SetData(self, x=None, y=None, Capacity=None):  # noqa: N802,N803
        """[YC] Supply the data directly as arrays (list, numpy). x defaults to 1..N."""
        if y is None and x is not None:
            x, y = None, x
        p = {"sheet": self._c._sheet, "name": self._c._name, "index": self._i, "y": _to_doubles(y), "append": False}
        if x is not None:
            p["x"] = _to_doubles(x)
        if Capacity:
            p["capacity"] = int(Capacity)
        _rq("chart.series_data", **p)

    def AppendData(self, x=None, y=None):  # noqa: N802
        """[YC] Append data points (streaming); the oldest points are dropped beyond Capacity."""
        if y is None and x is not None:
            x, y = None, x
        if not isinstance(y, (list, tuple)) and not hasattr(y, "dtype"):
            y = [y]
            x = None if x is None else [x]
        p = {"sheet": self._c._sheet, "name": self._c._name, "index": self._i, "y": _to_doubles(y), "append": True}
        if x is not None:
            p["x"] = _to_doubles(x)
        _rq("chart.series_data", **p)

    def BindTopic(self, Topic, Capacity=100000):  # noqa: N802,N803
        raise UnsupportedError("Series.BindTopic is not implemented yet: call AppendData from the stream callback (PLAN §9.4)")

    def Delete(self):  # noqa: N802
        _rq("chart.series_delete", sheet=self._c._sheet, name=self._c._name, index=self._i)

    def Select(self):  # noqa: N802
        _select_elem(self._c, _ELEM_SERIES, self._i)

    @property
    def MarkerStyle(self):  # noqa: N802
        return _MARKER_TO_XL.get(self._get().get("marker", -1), -4105)

    @MarkerStyle.setter
    def MarkerStyle(self, v):  # noqa: N802
        if int(v) not in _MARKER_IN:
            raise UnsupportedError(f"Unsupported marker style: {v}")
        self._set(marker=_MARKER_IN[int(v)])

    @property
    def MarkerSize(self):  # noqa: N802
        return int(self._get().get("marker_size") or 5)

    @MarkerSize.setter
    def MarkerSize(self, v):  # noqa: N802
        self._set(marker_size=float(v))

    @property
    def Explosion(self):  # noqa: N802
        """Pie and doughnut charts: every slice pulled out (% of the radius)."""
        return int(self._get().get("explosion", 0))

    @Explosion.setter
    def Explosion(self, v):  # noqa: N802
        self._set(explosion=int(v))

    @property
    def InvertIfNegative(self):  # noqa: N802
        return bool(self._get().get("invert_negative"))

    @InvertIfNegative.setter
    def InvertIfNegative(self, v):  # noqa: N802
        self._set(invert_negative=bool(v))

    @property
    def InvertColor(self):  # noqa: N802
        return self._get().get("invert_color", 0) or 0xFFFFFF

    @InvertColor.setter
    def InvertColor(self, v):  # noqa: N802
        self._set(invert_color=_excel_color(v), invert_negative=True)

    @property
    def MarkerBackgroundColor(self):  # noqa: N802
        return self._get().get("marker_fill", 0)

    @MarkerBackgroundColor.setter
    def MarkerBackgroundColor(self, v):  # noqa: N802
        self._set(marker_fill=_excel_color(v))

    @property
    def MarkerForegroundColor(self):  # noqa: N802
        return self._get().get("marker_line", 0)

    @MarkerForegroundColor.setter
    def MarkerForegroundColor(self, v):  # noqa: N802
        self._set(marker_line=_excel_color(v))

    @property
    def BubbleSizes(self):  # noqa: N802
        """Bubble charts: the range with each point's size."""
        return self._get().get("bubble_sizes", "")

    @BubbleSizes.setter
    def BubbleSizes(self, v):  # noqa: N802
        self._set(bubble_sizes=_addr(v))

    @property
    def Smooth(self):  # noqa: N802
        """A smoothed line through the points (line and scatter series)."""
        return bool(self._get().get("smooth"))

    @Smooth.setter
    def Smooth(self, v):  # noqa: N802
        self._set(smooth=bool(v))

    def _box(self, bit, v=None):
        flags = int(self._get().get("box_flags", 6))
        if v is None:
            return bool(flags & bit)
        self._set(box_flags=(flags | bit) if v else (flags & ~bit))

    @property
    def ShowInnerPoints(self):  # noqa: N802
        """Box and whisker: every value drawn as a point inside the whiskers."""
        return self._box(1)

    @ShowInnerPoints.setter
    def ShowInnerPoints(self, v):  # noqa: N802
        self._box(1, bool(v))

    @property
    def ShowOutlierPoints(self):  # noqa: N802
        return self._box(2)

    @ShowOutlierPoints.setter
    def ShowOutlierPoints(self, v):  # noqa: N802
        self._box(2, bool(v))

    @property
    def ShowMeanMarkers(self):  # noqa: N802
        return self._box(4)

    @ShowMeanMarkers.setter
    def ShowMeanMarkers(self, v):  # noqa: N802
        self._box(4, bool(v))

    @property
    def ShowMeanLine(self):  # noqa: N802
        """Box and whisker: a line joining the means of the boxes."""
        return self._box(8)

    @ShowMeanLine.setter
    def ShowMeanLine(self, v):  # noqa: N802
        self._box(8, bool(v))

    def _opt(self, key, default):
        return self._get().get(key, default)

    @property
    def QuartileCalculationInclusiveMedian(self):  # noqa: N802
        """Box and whisker: quartiles with the median included (QUARTILE.INC); False: exclusive median."""
        return bool(self._opt("inclusive_median", False))

    @QuartileCalculationInclusiveMedian.setter
    def QuartileCalculationInclusiveMedian(self, v):  # noqa: N802
        self._set(inclusive_median=bool(v))

    @property
    def ParentDataLabelOption(self):  # noqa: N802
        """Treemap Label Options: xlParentDataLabelOptionsNone (0), Banner (1) or Overlapping (2)."""
        return int(self._opt("parent_labels", 1))

    @ParentDataLabelOption.setter
    def ParentDataLabelOption(self, v):  # noqa: N802
        self._set(parent_labels=int(v))

    @property
    def GeoProjectionType(self):  # noqa: N802
        """Filled map: xlGeoProjectionTypeAutomatic, Mercator, Miller, Albers or Robinson."""
        return int(self._opt("geo_projection", 0))

    @GeoProjectionType.setter
    def GeoProjectionType(self, v):  # noqa: N802
        self._set(geo_projection=int(v))

    @property
    def GeoMappingLevel(self):  # noqa: N802
        """Filled map: the area shown (xlGeoMappingLevelAutomatic, DataOnly, CountryRegion, CountryRegionList, World)."""
        return int(self._opt("geo_area", 0))

    @GeoMappingLevel.setter
    def GeoMappingLevel(self, v):  # noqa: N802
        self._set(geo_area=int(v))

    @property
    def RegionLabelOption(self):  # noqa: N802
        """Filled map: region names xlRegionLabelOptionsNone, BestFitOnly or ShowAll."""
        return int(self._opt("geo_labels", 0))

    @RegionLabelOption.setter
    def RegionLabelOption(self, v):  # noqa: N802
        self._set(geo_labels=int(v))

    @property
    def SeriesColorGradientStyle(self):  # noqa: N802
        """Filled map Series Color: xlSeriesColorGradientStyleSequential (2-color) or Diverging (3-color)."""
        return int(self._opt("geo_colors", 0))

    @SeriesColorGradientStyle.setter
    def SeriesColorGradientStyle(self, v):  # noqa: N802
        self._set(geo_colors=int(v))

    @property
    def Format(self):  # noqa: N802
        return _SeriesFormat(self)


class _SeriesColor:
    def __init__(self, series, key="color"):
        self._s, self._k = series, key

    @property
    def RGB(self):  # noqa: N802
        return self._s._get().get(self._k, 0)

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        self._s._set(**{self._k: _excel_color(v)})


def _filled(series):
    """Whether a series is drawn filled (columns, bars, areas, slices, bubbles) rather than as a line."""
    t = series.ChartType
    return t in (51, 52, 53, 57, 58, 59, 1, 76, 77, 5, -4120, 15, 119, 118, 122, 121, 117, 120, 123)


class _SeriesLine:
    """Format.Line: a line series' line (its color is the series' color) or a filled series' border."""

    def __init__(self, series):
        self._s = series

    @property
    def Weight(self):  # noqa: N802
        return self._s._get().get("line_weight") or (0.75 if _filled(self._s) else 1.5)

    @Weight.setter
    def Weight(self, v):  # noqa: N802
        self._s._set(line_weight=float(v))

    @property
    def ForeColor(self):  # noqa: N802
        return _SeriesColor(self._s, "line_color" if _filled(self._s) else "color")

    @property
    def Visible(self):  # noqa: N802
        d = self._s._get()
        if d.get("no_line"):
            return 0
        return -1 if not _filled(self._s) or d.get("line_color") else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        props = {"no_line": not v}
        if v and _filled(self._s) and not self._s._get().get("line_color"):
            props["line_color"] = 0x404040
        self._s._set(**props)

    @property
    def DashStyle(self):  # noqa: N802
        """msoLineSolid, msoLineRoundDot, msoLineSquareDot, msoLineDash, msoLineDashDot, msoLineLongDash."""
        return _DASH_OUT.get(self._s._get().get("dash", 0), 1)

    @DashStyle.setter
    def DashStyle(self, v):  # noqa: N802
        if int(v) not in _DASH_IN:
            raise UnsupportedError(f"Unsupported dash style: {v}")
        self._s._set(dash=_DASH_IN[int(v)])

    @property
    def Style(self):  # noqa: N802
        """Compound type: msoLineSingle (1), msoLineThinThin (2, Double), msoLineThinThick (3), msoLineThickThin (4),
        msoLineThickBetweenThin (5, Triple)."""
        return _COMPOUND_OUT.get(self._s._get().get("compound", 0), 1)

    @Style.setter
    def Style(self, v):  # noqa: N802
        if int(v) not in _COMPOUND_IN:
            raise UnsupportedError(f"Unsupported line style: {v}")
        self._s._set(compound=_COMPOUND_IN[int(v)])

    @property
    def BeginArrowheadStyle(self):  # noqa: N802
        """msoArrowheadNone (1), Triangle (2), Open (3), Stealth (4), Diamond (5), Oval (6)."""
        return int(self._s._get().get("begin_arrow", 0)) + 1

    @BeginArrowheadStyle.setter
    def BeginArrowheadStyle(self, v):  # noqa: N802
        self._s._set(begin_arrow=max(0, min(5, int(v) - 1)))

    @property
    def EndArrowheadStyle(self):  # noqa: N802
        return int(self._s._get().get("end_arrow", 0)) + 1

    @EndArrowheadStyle.setter
    def EndArrowheadStyle(self, v):  # noqa: N802
        self._s._set(end_arrow=max(0, min(5, int(v) - 1)))


class _SeriesBackColor:
    """Fill.BackColor: a gradient's second color or a pattern's background (.RGB)."""

    def __init__(self, series):
        self._s = series

    @property
    def RGB(self):  # noqa: N802
        d = self._s._get()
        v = d.get("pattern_back") if d.get("fill_type") == 3 else d.get("grad_color2")
        return v or 0xFFFFFF

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        d = self._s._get()
        self._s._set(**{"pattern_back" if d.get("fill_type") == 3 else "grad_color2": int(v) & 0xFFFFFF})


class _SeriesFill:
    """Format.Fill: solid, gradient, picture or texture, pattern fill; the color, transparency and whether there is
    one."""

    def __init__(self, series):
        self._s = series

    @property
    def ForeColor(self):  # noqa: N802
        return _SeriesColor(self._s)

    @property
    def Transparency(self):  # noqa: N802
        return self._s._get().get("transparency", 0) / 100.0

    @Transparency.setter
    def Transparency(self, v):  # noqa: N802
        self._s._set(transparency=int(round(float(v) * 100)))

    @property
    def Visible(self):  # noqa: N802
        return 0 if self._s._get().get("no_fill") else -1

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._s._set(no_fill=not v)

    def Solid(self):  # noqa: N802
        self._s._set(no_fill=False, fill_type=0)

    @property
    def BackColor(self):  # noqa: N802
        return _SeriesBackColor(self._s)

    @property
    def Type(self):  # noqa: N802
        """msoFillSolid (1), msoFillPatterned (2), msoFillGradient (3), msoFillPicture (6)."""
        return {0: 1, 1: 3, 2: 6, 3: 2}.get(int(self._s._get().get("fill_type", 0)), 1)

    def TwoColorGradient(self, Style=1, Variant=1):  # noqa: N802,N803
        """A gradient from ForeColor to BackColor: msoGradientHorizontal (1, top to bottom), msoGradientVertical (2,
        left to right), msoGradientDiagonalUp (3), msoGradientDiagonalDown (4); Variant 2 runs it the other way."""
        angle = {1: 90, 2: 0, 3: 315, 4: 45}.get(int(Style), 90)
        if int(Variant) == 2:
            angle = (angle + 180) % 360
        self._s._set(no_fill=False, fill_type=1, grad_angle=angle)

    @property
    def GradientAngle(self):  # noqa: N802
        return float(self._s._get().get("grad_angle", 90))

    @GradientAngle.setter
    def GradientAngle(self, v):  # noqa: N802
        self._s._set(grad_angle=int(round(float(v))) % 360)

    def Patterned(self, Pattern):  # noqa: N802,N803
        """A pattern of ForeColor on BackColor: msoPattern5Percent (1), 10Percent (2), 25Percent (4), 50Percent (7),
        LightHorizontal (19), LightVertical (20), LightDownwardDiagonal (21), LightUpwardDiagonal (22), DarkHorizontal
        (13), DarkVertical (14), SmallGrid (23), DiagonalCross (25... any other pattern draws as the nearest kind)."""
        self._s._set(no_fill=False, fill_type=3, pattern=_PATTERN_IN.get(int(Pattern), 0))

    @property
    def Pattern(self):  # noqa: N802
        return _PATTERN_OUT.get(int(self._s._get().get("pattern", 0)), 1)

    def UserPicture(self, PictureFile=None, *args):  # noqa: N802,N803
        """Picture fill: the picture stretched over each column, bar, slice or area."""
        import os

        self._s._set(no_fill=False, fill_type=2, picture_tile=False, picture_file=os.path.abspath(str(PictureFile)))

    def _tile(self, key, default, v=None, scale=1.0):
        if v is None:
            return self._s._get().get(key, default) / scale
        self._s._set(**{key: v * scale})

    TextureTile = property(lambda self: bool(self._s._get().get("picture_tile")), lambda self, v: self._s._set(picture_tile=bool(v)),
                           doc="Tile picture as texture (False: stretched)")
    TextureOffsetX = property(lambda self: self._tile("tile_ox", 0.0), lambda self, v: self._tile("tile_ox", 0.0, float(v)))
    TextureOffsetY = property(lambda self: self._tile("tile_oy", 0.0), lambda self, v: self._tile("tile_oy", 0.0, float(v)))
    TextureHorizontalScale = property(lambda self: self._tile("tile_sx", 100.0, scale=100.0), lambda self, v: self._tile("tile_sx", 100.0, float(v), 100.0),
                                      doc="1 = the picture's own size")
    TextureVerticalScale = property(lambda self: self._tile("tile_sy", 100.0, scale=100.0), lambda self, v: self._tile("tile_sy", 100.0, float(v), 100.0))
    TextureAlignment = property(lambda self: int(self._s._get().get("tile_align", 0)), lambda self, v: self._s._set(tile_align=int(v)),
                                doc="msoTextureTopLeft (0) ... msoTextureBottomRight (8)")
    TextureMirrorType = property(lambda self: int(self._s._get().get("tile_mirror", 0)), lambda self, v: self._s._set(tile_mirror=int(v)),
                                 doc="[YC] Mirror type: 0 none, 1 horizontal, 2 vertical, 3 both")

    def UserTextured(self, TextureFile):  # noqa: N802,N803
        """Texture fill: the picture tiled at its own size."""
        import os

        self._s._set(no_fill=False, fill_type=2, picture_tile=True, picture_file=os.path.abspath(str(TextureFile)))


class _SeriesFormat:
    def __init__(self, series):
        self._s = series

    @property
    def Line(self):  # noqa: N802
        return _SeriesLine(self._s)

    @property
    def Fill(self):  # noqa: N802
        return _SeriesFill(self._s)


class _LabelsTextRange:
    def __init__(self, labels):
        self._l = labels

    def InsertChartField(self, ChartFieldType, Formula=None, Position=None):  # noqa: N802,N803
        """msoChartFieldRange with Formula "=Sheet1!$C$2:$C$5": the labels show those cells (Value From Cells)."""
        if Formula is None:
            raise UnsupportedError("InsertChartField needs the cells' Formula")
        self._l._s._set(label_range=_addr(str(Formula).lstrip("=")), data_labels=True,
                        label_show=int(self._l._s._get().get("label_show", 4)) | 32)


class _LabelsTextFrame:
    def __init__(self, labels):
        self.TextRange = _LabelsTextRange(labels)


class _LabelBoxColor:
    """.ForeColor of a data label's box (.RGB); 0 in the model = none."""

    def __init__(self, read, write):
        self._read, self._write = read, write

    @property
    def RGB(self):  # noqa: N802
        argb = self._read()
        return _from_argb(argb) if argb else 0xFFFFFF

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        self._write(_argb(v))


class _LabelBoxPart:
    """Format.Fill / Format.Line of data labels: ForeColor.RGB, Visible."""

    def __init__(self, read, write):
        self._read, self._write = read, write

    @property
    def ForeColor(self):  # noqa: N802
        return _LabelBoxColor(self._read, self._write)

    @property
    def Visible(self):  # noqa: N802
        return -1 if self._read() else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        if not v:
            self._write(0)
        elif not self._read():
            self._write(_argb(0xFFFFFF))

    def Solid(self):  # noqa: N802
        self.Visible = True


class _LabelsFormat:
    def __init__(self, labels):
        self.TextFrame2 = _LabelsTextFrame(labels)
        self._s = labels._s

    @property
    def Fill(self):  # noqa: N802
        return _LabelBoxPart(lambda: self._s._get().get("label_fill", 0), lambda v: self._s._set(label_fill=v))

    @property
    def Line(self):  # noqa: N802
        return _LabelBoxPart(lambda: self._s._get().get("label_line", 0), lambda v: self._s._set(label_line=v))


class _LabelFormat:
    """One data label's Format: its own Fill and Line (its series' until set)."""

    def __init__(self, label):
        self._l = label

    def _part(self, key):
        lab = self._l

        def read():
            return lab._pf().get(key) or lab._s._get().get(key, 0)

        def write(v):
            lab._s._set(point_set={"index": lab._p, key: v})

        return _LabelBoxPart(read, write)

    @property
    def Fill(self):  # noqa: N802
        return self._part("label_fill")

    @property
    def Line(self):  # noqa: N802
        return self._part("label_line")


class _DataLabels:
    """Series.DataLabels(): what the labels show, where, and their number format (Format Data Labels)."""

    def __init__(self, series: "Series"):
        self._s = series

    def _bit(self, bit, v=None):
        show = int(self._s._get().get("label_show", 4))
        if v is None:
            return bool(show & bit)
        self._s._set(label_show=(show | bit) if v else (show & ~bit), data_labels=True)

    @property
    def ShowValue(self):  # noqa: N802
        return self._s.HasDataLabels and self._bit(4)

    @ShowValue.setter
    def ShowValue(self, v):  # noqa: N802
        if not v and not (int(self._s._get().get("label_show", 4)) & ~4):
            self._s.HasDataLabels = False
            return
        self._bit(4, bool(v))

    @property
    def ShowSeriesName(self):  # noqa: N802
        return self._bit(1)

    @ShowSeriesName.setter
    def ShowSeriesName(self, v):  # noqa: N802
        self._bit(1, bool(v))

    @property
    def ShowCategoryName(self):  # noqa: N802
        return self._bit(2)

    @ShowCategoryName.setter
    def ShowCategoryName(self, v):  # noqa: N802
        self._bit(2, bool(v))

    @property
    def ShowPercentage(self):  # noqa: N802
        return self._bit(8)

    @ShowPercentage.setter
    def ShowPercentage(self, v):  # noqa: N802
        self._bit(8, bool(v))

    @property
    def ShowRange(self):  # noqa: N802
        """Value From Cells (the cells set with Format.TextFrame2.TextRange.InsertChartField)."""
        return self._bit(32)

    @ShowRange.setter
    def ShowRange(self, v):  # noqa: N802
        self._bit(32, bool(v))

    @property
    def Separator(self):  # noqa: N802
        return self._s._get().get("label_sep", ", ")

    @Separator.setter
    def Separator(self, v):  # noqa: N802
        self._s._set(label_sep=str(v))

    @property
    def Position(self):  # noqa: N802
        """xlLabelPositionCenter, InsideEnd, InsideBase, OutsideEnd, Left, Right, Above, Below, BestFit."""
        p = self._s._get().get("label_pos", -1)
        return _LABEL_POS_OUT.get(p, 2 if p < 0 else -4108)

    @Position.setter
    def Position(self, v):  # noqa: N802
        if int(v) not in _LABEL_POS_IN:
            raise UnsupportedError(f"Unsupported label position: {v}")
        self._s._set(label_pos=_LABEL_POS_IN[int(v)], data_labels=True)

    @property
    def NumberFormat(self):  # noqa: N802
        return self._s._get().get("label_format") or "General"

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        v = str(v)
        self._s._set(label_format="" if v.lower() == "general" else v)

    @property
    def NumberFormatLinked(self):  # noqa: N802
        return not self._s._get().get("label_format")

    @NumberFormatLinked.setter
    def NumberFormatLinked(self, v):  # noqa: N802
        if v:
            self._s._set(label_format="")

    @property
    def Format(self):  # noqa: N802
        return _LabelsFormat(self)

    def Select(self):  # noqa: N802
        _select_elem(self._s._c, _ELEM_LABELS, self._s._i)

    def Delete(self):  # noqa: N802
        self._s.HasDataLabels = False

    @property
    def ShowLegendKey(self):  # noqa: N802
        return self._bit(16)

    @ShowLegendKey.setter
    def ShowLegendKey(self, v):  # noqa: N802
        self._bit(16, bool(v))

    @property
    def ShowLeaderLines(self):  # noqa: N802
        """Leader lines from labels dragged away to their points (Series.HasLeaderLines)."""
        return bool(self._s._get().get("leader_lines", True))

    @ShowLeaderLines.setter
    def ShowLeaderLines(self, v):  # noqa: N802
        self._s._set(leader_lines=bool(v))

    @property
    def Font(self):  # noqa: N802
        return _ChartFont(self._s, "label_font", 9)

    @property
    def Count(self):  # noqa: N802
        return self._s.Points().Count if self._s.HasDataLabels else 0

    def __call__(self, Index=None):  # noqa: N803
        # DataLabels() is the collection, DataLabels(i) one label (as Excel)
        return self if Index is None else DataLabel(self._s, int(Index) - 1)

    def Item(self, Index):  # noqa: N802,N803
        return DataLabel(self._s, int(Index) - 1)


class DataLabel:
    """Series.DataLabels(i) / Point.DataLabel: one data label (shown or not, dragged away with Left / Top offsets)."""

    def __init__(self, series, point):
        self._s, self._p = series, int(point)  # 0-based point

    def _pf(self):
        for q in self._s._get().get("points", []):
            if q.get("index") == self._p:
                return q
        return {}

    def Select(self):  # noqa: N802
        _select_elem(self._s._c, _ELEM_DATA_LABEL, self._s._i, point=self._p)

    def Delete(self):  # noqa: N802
        self._s._set(point_set={"index": self._p, "label": 0})

    def _read(self):
        q = self._pf()
        return _font_of(q, "label_font") if q.get("label_font_set") else _font_of(self._s._get(), "label_font")

    def _write(self, font):
        self._s._set(point_set=dict({"index": self._p}, **{f"label_font_{k}": v for k, v in font.items() if v is not None}))

    @property
    def Font(self):  # noqa: N802
        """This label's own font (Format Data Label > Text Options); its series' labels' until it is set."""
        return _ChartFont(_OwnFont(self._read, self._write), "f", 9)

    @property
    def Text(self):  # noqa: N802
        """The label's text: what was typed, a linked cell's text, else its value (Label Contains)."""
        own = self._pf().get("label_text", "")
        if own.startswith("="):
            from . import excel

            try:
                return str(excel.Range(own[1:]).Text)
            except Exception:  # noqa: BLE001
                return own
        if own:
            return own
        vals = self._s.Values
        v = vals[self._p] if isinstance(vals, (list, tuple)) and self._p < len(vals) else ""
        return f"{v:g}" if isinstance(v, float) else str(v)

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._s._set(point_set={"index": self._p, "label_text": str(v)})

    Caption = Text

    @property
    def Format(self):  # noqa: N802
        """The label's own Fill and Line (Format Data Label > Fill & Line)."""
        return _LabelFormat(self)

    @property
    def Formula(self):  # noqa: N802
        """"=Sheet1!$C$2" when the label shows a cell (it follows the cell), else its own text."""
        return self._pf().get("label_text", "")

    @Formula.setter
    def Formula(self, v):  # noqa: N802
        v = str(v)
        if v and not v.startswith("="):
            v = "=" + v
        self._s._set(point_set={"index": self._p, "label_text": "=" + _addr(v[1:]) if v else ""})

    def _bit(self, bit, v=None):
        q = self._pf()
        show = q.get("label_show", -1)
        if show is None or show < 0:
            show = int(self._s._get().get("label_show", 4))
        if v is None:
            return bool(show & bit)
        self._s._set(point_set={"index": self._p, "label": 1, "label_show": (show | bit) if v else (show & ~bit)})

    ShowValue = property(lambda self: self._bit(4), lambda self, v: self._bit(4, bool(v)))
    ShowSeriesName = property(lambda self: self._bit(1), lambda self, v: self._bit(1, bool(v)))
    ShowCategoryName = property(lambda self: self._bit(2), lambda self, v: self._bit(2, bool(v)))
    ShowPercentage = property(lambda self: self._bit(8), lambda self, v: self._bit(8, bool(v)))
    ShowLegendKey = property(lambda self: self._bit(16), lambda self, v: self._bit(16, bool(v)))

    @property
    def Position(self):  # noqa: N802
        pos = self._pf().get("label_pos", -1)
        if pos is None or pos < 0:
            pos = self._s._get().get("label_pos", -1)
        return _LABEL_POS_OUT.get(pos, 5 if pos < 0 else -4108)

    @Position.setter
    def Position(self, v):  # noqa: N802
        if int(v) not in _LABEL_POS_IN:
            raise UnsupportedError(f"Unsupported label position: {v}")
        self._s._set(point_set={"index": self._p, "label": 1, "label_pos": _LABEL_POS_IN[int(v)]})

    @property
    def Offset(self):  # noqa: N802
        """(dx, dy): how far the label was dragged from its place, as fractions of the chart's width and height."""
        q = self._pf()
        return (float(q.get("label_dx", 0.0)), float(q.get("label_dy", 0.0)))

    @Offset.setter
    def Offset(self, v):  # noqa: N802
        self._s._set(point_set={"index": self._p, "label_dx": float(v[0]), "label_dy": float(v[1])})


class Trendline:
    """Series.Trendlines(i): the first trendline, or one of the others a series can have (Excel allows several)."""

    def __init__(self, series: "Series", index: int = 1):
        self._s, self._k = series, int(index)  # 1-based

    def __repr__(self):
        return f"<Trendline {self._k} of {self._s!r}>"

    def _g(self):
        d = self._s._get()
        if self._k == 1:
            if not d.get("trend"):
                raise SubscriptOutOfRangeError("The trendline no longer exists")
            return d
        more = d.get("more_trends", [])
        if not d.get("trend") or self._k - 2 >= len(more):
            raise SubscriptOutOfRangeError("The trendline no longer exists")
        return more[self._k - 2]

    def _put(self, **p):
        if self._k == 1:
            self._s._set(**p)
            return
        more = [dict(t) for t in self._s._get().get("more_trends", [])]
        if self._k - 2 >= len(more):
            raise SubscriptOutOfRangeError("The trendline no longer exists")
        more[self._k - 2].update(p)
        self._s._set(more_trends=more)

    @property
    def Index(self):  # noqa: N802
        return self._k

    @property
    def Type(self):  # noqa: N802
        return _TREND_OUT.get(self._g()["trend"], -4132)

    @Type.setter
    def Type(self, v):  # noqa: N802
        k = _TREND_IN.get(int(v))
        if k is None:
            raise UnsupportedError(f"Unsupported trendline type: {v}")
        self._put(trend=k)

    @property
    def Order(self):  # noqa: N802
        return self._g()["trend_order"]

    @Order.setter
    def Order(self, v):  # noqa: N802
        self._put(trend_order=int(v))

    @property
    def Period(self):  # noqa: N802
        return self._g()["trend_period"]

    @Period.setter
    def Period(self, v):  # noqa: N802
        self._put(trend_period=int(v))

    @property
    def Forward(self):  # noqa: N802
        return self._g()["trend_forward"]

    @Forward.setter
    def Forward(self, v):  # noqa: N802
        self._put(trend_forward=float(v))

    @property
    def Backward(self):  # noqa: N802
        return self._g()["trend_backward"]

    @Backward.setter
    def Backward(self, v):  # noqa: N802
        self._put(trend_backward=float(v))

    @property
    def DisplayEquation(self):  # noqa: N802
        return self._g()["trend_eq"]

    @DisplayEquation.setter
    def DisplayEquation(self, v):  # noqa: N802
        self._put(trend_eq=bool(v))

    @property
    def DisplayRSquared(self):  # noqa: N802
        return self._g()["trend_r2"]

    @DisplayRSquared.setter
    def DisplayRSquared(self, v):  # noqa: N802
        self._put(trend_r2=bool(v))

    @property
    def Intercept(self):  # noqa: N802
        """Set Intercept: where the trendline crosses the vertical axis (linear, polynomial, exponential)."""
        return float(self._g().get("trend_icept", 0.0))

    @Intercept.setter
    def Intercept(self, v):  # noqa: N802
        self._put(trend_icept=float(v), trend_icept_on=True)

    @property
    def InterceptIsAuto(self):  # noqa: N802
        return not self._g().get("trend_icept_on", False)

    @InterceptIsAuto.setter
    def InterceptIsAuto(self, v):  # noqa: N802
        self._put(trend_icept_on=not v)

    @property
    def Name(self):  # noqa: N802
        """The name in the legend: Automatic ("Linear (Sales)") or a custom one."""
        g = self._g()
        if g.get("trend_name"):
            return g["trend_name"]
        names = {1: "Linear", 2: "Expon.", 3: "Log.", 4: "Poly.", 5: "Power", 6: "Mov. Avg."}
        k = g["trend"]
        n = f"{g.get('trend_period', 2)} per. {names[6]}" if k == 6 else names.get(k, "Linear")
        return f"{n} ({self._s.Name})"

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._put(trend_name=str(v))

    @property
    def NameIsAuto(self):  # noqa: N802
        return not self._g().get("trend_name")

    @NameIsAuto.setter
    def NameIsAuto(self, v):  # noqa: N802
        if v:
            self._put(trend_name="")

    def Delete(self):  # noqa: N802
        d = self._s._get()
        more = [dict(t) for t in d.get("more_trends", [])]
        if self._k == 1:
            if more:  # the next trendline becomes the first
                first = more.pop(0)
                first.setdefault("trend_name", "")
                first.setdefault("trend_icept_on", False)
                first.setdefault("trend_icept", 0.0)
                self._s._set(more_trends=more, **first)
            else:
                self._s._set(trend=0)
        elif self._k - 2 < len(more):
            del more[self._k - 2]
            self._s._set(more_trends=more)

    def Select(self):  # noqa: N802
        _select_elem(self._s._c, _ELEM_TREND, self._s._i, point=self._k - 1)


class _Trendlines:
    def __init__(self, series: "Series"):
        self._s = series

    @property
    def Count(self):  # noqa: N802
        d = self._s._get()
        return (1 + len(d.get("more_trends", []))) if d.get("trend") else 0

    def __len__(self):
        return self.Count

    def Item(self, Index=1):  # noqa: N802,N803
        if not 1 <= int(Index) <= self.Count:
            raise SubscriptOutOfRangeError("Trendline index out of range")
        return Trendline(self._s, int(Index))

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([Trendline(self._s, k) for k in range(1, self.Count + 1)])

    def Add(self, Type=-4132, Order=None, Period=None, Forward=None, Backward=None, Intercept=None,  # noqa: N802,N803
            DisplayEquation=None, DisplayRSquared=None, Name=None):
        """A new trendline: the series' first, or one more beside those it has (each with its own legend entry)."""
        k = _TREND_IN.get(int(Type))
        if k is None:
            raise UnsupportedError(f"Unsupported trendline type: {Type}")
        p = {"trend": k, "trend_forward": float(Forward or 0), "trend_backward": float(Backward or 0),
             "trend_eq": bool(DisplayEquation), "trend_r2": bool(DisplayRSquared)}
        if Order is not None:
            p["trend_order"] = int(Order)
        if Period is not None:
            p["trend_period"] = int(Period)
        if Intercept is not None:
            p["trend_icept"] = float(Intercept)
            p["trend_icept_on"] = True
        if Name is not None:
            p["trend_name"] = str(Name)
        d = self._s._get()
        if not d.get("trend"):
            self._s._set(**p)
            return Trendline(self._s, 1)
        full = {"trend_order": 2, "trend_period": 2, "trend_name": "", "trend_icept_on": False, "trend_icept": 0.0}
        full.update(p)
        more = [dict(t) for t in d.get("more_trends", [])] + [full]
        self._s._set(more_trends=more)
        return Trendline(self._s, len(more) + 1)


class _ErrorBars:
    """Series.ErrorBars: the Y error bars (a scatter chart's X ones: Series.XErrorBars, [YC])."""

    def __init__(self, series: "Series", x: bool = False):
        self._s, self._x = series, x

    def _k(self, key):
        return ("x" + key) if self._x else key

    @property
    def Type(self):  # noqa: N802
        """[YC] The error bar type (XlErrorBarType); 0 when there are none."""
        return _ERR_OUT.get(self._s._get().get(self._k("err"), 0), 0)

    @property
    def Amount(self):  # noqa: N802
        """[YC] The fixed value, percentage or number of standard deviations (custom: the positive amounts' range or array)."""
        d = self._s._get()
        return d.get(self._k("err_plus"), "") if d.get(self._k("err")) == 5 else d.get(self._k("err_amount"), 1.0)

    @property
    def MinusValues(self):  # noqa: N802
        """[YC] Custom error bars: the negative amounts' range or array."""
        return self._s._get().get(self._k("err_minus"), "")

    @property
    def EndStyle(self):  # noqa: N802
        """xlCap (1) or xlNoCap (2): the T at the ends of the bars."""
        return 1 if self._s._get().get("err_cap", True) else 2

    @EndStyle.setter
    def EndStyle(self, v):  # noqa: N802
        self._s._set(err_cap=int(v) != 2)

    @property
    def Include(self):  # noqa: N802
        """[YC] xlErrorBarIncludeBoth, xlErrorBarIncludePlusValues or xlErrorBarIncludeMinusValues."""
        return self._s._get().get(self._k("err_include"), 1)

    def Delete(self):  # noqa: N802
        self._s._set(**{self._k("err"): 0})

    def Select(self):  # noqa: N802
        _select_elem(self._s._c, _ELEM_ERRBARS, self._s._i, point=1 if self._x else None)


class Point:
    def __init__(self, series: "Series", index: int):
        self._s, self._i = series, index  # 0-based

    @property
    def DataLabel(self):  # noqa: N802
        """The point's data label: its own Text, Formula, Font, Show... and Position (Format Data Label)."""
        return DataLabel(self._s, self._i)

    def __repr__(self):
        return f"<Point {self._i + 1} of {self._s!r}>"

    @property
    def IsTotal(self):  # noqa: N802
        """Waterfall charts: the point is a total (its column starts at zero)."""
        return self._i in self._s._get().get("totals", [])

    @IsTotal.setter
    def IsTotal(self, v):  # noqa: N802
        tot = [t for t in self._s._get().get("totals", []) if t != self._i]
        if v:
            tot.append(self._i)
        self._s._set(totals=sorted(tot))

    def _pf(self):
        for p in self._s._get().get("points", []):
            if p.get("index") == self._i:
                return p
        return {"index": self._i, "fill": 0, "line": 0, "explosion": -1, "label": -1}

    def _setp(self, **kw):
        kw["index"] = self._i
        self._s._set(point_set=kw)

    @property
    def Explosion(self):  # noqa: N802
        """Pie slices: how far this slice is pulled out (% of the radius)."""
        e = self._pf()["explosion"]
        return e if e >= 0 else self._s.Explosion

    @Explosion.setter
    def Explosion(self, v):  # noqa: N802
        self._setp(explosion=int(v))

    @property
    def HasDataLabel(self):  # noqa: N802
        lab = self._pf()["label"]
        return self._s.HasDataLabels if lab < 0 else lab == 1

    @HasDataLabel.setter
    def HasDataLabel(self, v):  # noqa: N802
        self._setp(label=1 if v else 0)

    @property
    def MarkerBackgroundColor(self):  # noqa: N802
        return self._pf()["fill"]

    @MarkerBackgroundColor.setter
    def MarkerBackgroundColor(self, v):  # noqa: N802
        self._setp(fill=_excel_color(v))

    @property
    def Format(self):  # noqa: N802
        return _PointFormat(self)

    def Select(self):  # noqa: N802
        _select_elem(self._s._c, _ELEM_POINT, self._s._i, point=self._i)


class _PointColor:
    def __init__(self, point, key):
        self._p, self._k = point, key

    @property
    def RGB(self):  # noqa: N802
        return self._p._pf()[self._k]

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        self._p._setp(**{self._k: _excel_color(v)})


class _PointFill:
    def __init__(self, point, key):
        self._p, self._k = point, key

    @property
    def ForeColor(self):  # noqa: N802
        return _PointColor(self._p, self._k)

    @property
    def Visible(self):  # noqa: N802
        return -1


class _PointFormat:
    def __init__(self, point):
        self.Fill = _PointFill(point, "fill")
        self.Line = _PointFill(point, "line")


class _Points:
    def __init__(self, series: "Series"):
        self._s = series

    @property
    def Count(self):  # noqa: N802
        from . import excel

        d = self._s._get()
        if d.get("python_data"):
            return d.get("count", 0)
        try:
            return excel.Range(d["values"]).Count if d["values"] else 0
        except Exception:  # noqa: BLE001
            return 0

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        if int(Index) < 1:
            raise SubscriptOutOfRangeError("Point index out of range")
        return Point(self._s, int(Index) - 1)

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self


class _StockElem:
    """A stock chart's Up Bars, Down Bars or High-Low Lines (ChartGroup.UpBars / DownBars / HiLoLines), and a 3-D
    chart's Floor and Walls (Chart.Floor / Walls): Format.Fill or Format.Line, Interior / Border, Select, Delete."""

    def __init__(self, chart, key, auto_rgb, elem, line=False, has_key=None):
        self._c, self._k, self._auto, self._e, self._line, self._has = chart, key, auto_rgb, elem, line, has_key

    @property
    def Format(self):  # noqa: N802
        el = self

        class _F:
            @property
            def Fill(self):  # noqa: N802
                return _ChartFill(el._c, el._k, el._auto)

            @property
            def Line(self):  # noqa: N802
                return _ElemLine(el._c, el._k)

        return _F()

    @property
    def Interior(self):  # noqa: N802
        return _ChartInterior(self._c, self._k, self._auto)

    @property
    def Border(self):  # noqa: N802
        return _ChartInterior(self._c, self._k, self._auto)

    def Select(self):  # noqa: N802
        _select_elem(self._c, self._e)

    def Delete(self):  # noqa: N802
        if self._has:
            self._c._set(**{self._has: False})
        else:
            self._c._set(**{self._k: 0})

    def ClearFormats(self):  # noqa: N802
        self._c._set(**{self._k: 0})


_ELEM_UP_BARS, _ELEM_DOWN_BARS, _ELEM_HILO, _ELEM_FLOOR, _ELEM_WALLS = 17, 18, 19, 20, 21


class ChartGroup:
    """Chart.ChartGroups(1): histogram bins; GapWidth, Overlap (columns and bars), FirstSliceAngle (pie, doughnut),
    DoughnutHoleSize."""

    def __init__(self, chart: "Chart"):
        self._c = chart

    @property
    def GapWidth(self):  # noqa: N802
        if self._of_pie():
            return int(self._c._get().get("pie_gap", 100))
        return int(self._c._get().get("gap_width", 219))

    @GapWidth.setter
    def GapWidth(self, v):  # noqa: N802
        if self._of_pie():
            self._c._set(pie_gap=int(v))
        else:
            self._c._set(gap_width=int(v))

    def _of_pie(self):
        return self._c._get().get("type") in (46, 47)

    # pie of pie / bar of pie: Series Options (Split Series By, Values in second plot, Second Plot Size, Gap Width)
    @property
    def SplitType(self):  # noqa: N802
        """xlSplitByPosition, xlSplitByValue, xlSplitByPercentValue or xlSplitByCustomSplit."""
        return int(self._c._get().get("split_type", 1))

    @SplitType.setter
    def SplitType(self, v):  # noqa: N802
        self._c._set(split_type=int(v))

    @property
    def SplitValue(self):  # noqa: N802
        """By position: how many of the last points; by value / percent: the points under it."""
        d = self._c._get()
        v = float(d.get("split_value", 0))
        if v <= 0:
            return {1: 3, 3: 10}.get(int(d.get("split_type", 1)), 0)
        return int(v) if v == int(v) else v

    @SplitValue.setter
    def SplitValue(self, v):  # noqa: N802
        self._c._set(split_value=float(v))

    @property
    def SecondPlotSize(self):  # noqa: N802
        return int(self._c._get().get("second_size", 75))

    @SecondPlotSize.setter
    def SecondPlotSize(self, v):  # noqa: N802
        self._c._set(second_size=int(v))

    @property
    def PieGapWidth(self):  # noqa: N802
        """[YC] Pie of pie / bar of pie: the gap between the two plots (ChartGroup.GapWidth reads it on those charts)."""
        return int(self._c._get().get("pie_gap", 100))

    @PieGapWidth.setter
    def PieGapWidth(self, v):  # noqa: N802
        self._c._set(pie_gap=int(v))

    # stock charts: up / down bars and high-low lines
    @property
    def HasUpDownBars(self):  # noqa: N802
        return bool(self._c._get().get("up_down_bars", True))

    @HasUpDownBars.setter
    def HasUpDownBars(self, v):  # noqa: N802
        self._c._set(up_down_bars=bool(v))

    @property
    def HasHiLoLines(self):  # noqa: N802
        return bool(self._c._get().get("hi_lo_lines", True))

    @HasHiLoLines.setter
    def HasHiLoLines(self, v):  # noqa: N802
        self._c._set(hi_lo_lines=bool(v))

    @property
    def UpBars(self):  # noqa: N802
        return _StockElem(self._c, "up_fill", 0x0000FF, _ELEM_UP_BARS, has_key="up_down_bars")

    @property
    def DownBars(self):  # noqa: N802
        return _StockElem(self._c, "down_fill", 0x00B050, _ELEM_DOWN_BARS, has_key="up_down_bars")

    @property
    def HiLoLines(self):  # noqa: N802
        return _StockElem(self._c, "hilo_line", 0x404040, _ELEM_HILO, line=True, has_key="hi_lo_lines")

    @property
    def Overlap(self):  # noqa: N802
        return int(self._c._get().get("overlap", -27))

    @Overlap.setter
    def Overlap(self, v):  # noqa: N802
        self._c._set(overlap=int(v))

    @property
    def FirstSliceAngle(self):  # noqa: N802
        return int(self._c._get().get("first_slice_angle", 0))

    @FirstSliceAngle.setter
    def FirstSliceAngle(self, v):  # noqa: N802
        self._c._set(first_slice_angle=int(v))

    @property
    def DoughnutHoleSize(self):  # noqa: N802
        return int(self._c._get().get("hole_size", 50))

    @DoughnutHoleSize.setter
    def DoughnutHoleSize(self, v):  # noqa: N802
        self._c._set(hole_size=int(v))

    @property
    def BinsType(self):  # noqa: N802
        d = self._c._get()
        if d.get("bins_by_category"):
            return 1
        return 3 if d.get("bin_width") else (4 if d.get("bin_count") else 0)

    @BinsType.setter
    def BinsType(self, v):  # noqa: N802
        if int(v) == 0:
            self._c._set(bin_width=0.0, bin_count=0, bins_by_category=False)
        elif int(v) == 1:
            self._c._set(bins_by_category=True)
        elif int(v) not in (3, 4):
            raise UnsupportedError("Only xlBinsTypeAutomatic, xlBinsTypeCategorical, xlBinsTypeBinSize and xlBinsTypeBinCount are supported")

    @property
    def BinWidthValue(self):  # noqa: N802
        return self._c._get().get("bin_width", 0.0)

    @BinWidthValue.setter
    def BinWidthValue(self, v):  # noqa: N802
        self._c._set(bin_width=float(v), bin_count=0)

    @property
    def BinsCountValue(self):  # noqa: N802
        return self._c._get().get("bin_count", 0)

    @BinsCountValue.setter
    def BinsCountValue(self, v):  # noqa: N802
        self._c._set(bin_count=int(v), bin_width=0.0)

    @property
    def VaryByCategories(self):  # noqa: N802
        """A color per slice (pies, doughnuts: on by default) or per point (one series of columns or bars: Vary colors
        by point, off by default)."""
        d = self._c._get()
        ss = d["series"]
        if d.get("type") in (7, 8, 26):
            return bool(ss[0].get("vary_colors", True)) if ss else True
        return bool(ss[0].get("vary_points", False)) if ss else False

    @VaryByCategories.setter
    def VaryByCategories(self, v):  # noqa: N802
        pie = self._c._get().get("type") in (7, 8, 26)
        for i in range(len(self._c._get()["series"])):
            Series(self._c, i)._set(**{"vary_colors" if pie else "vary_points": bool(v)})

    @property
    def ShowConnectorLines(self):  # noqa: N802
        """[YC] Waterfall: Format Data Series > "Show connector lines" (True by default)."""
        return bool(self._c._get().get("connectors", True))

    @ShowConnectorLines.setter
    def ShowConnectorLines(self, v):  # noqa: N802
        self._c._set(connectors=bool(v))

    @property
    def BinsOverflowEnabled(self):  # noqa: N802
        return bool(self._c._get().get("bins_overflow"))

    @BinsOverflowEnabled.setter
    def BinsOverflowEnabled(self, v):  # noqa: N802
        self._c._set(bins_overflow=bool(v))

    @property
    def BinsOverflowValue(self):  # noqa: N802
        return self._c._get().get("overflow_at", 0.0)

    @BinsOverflowValue.setter
    def BinsOverflowValue(self, v):  # noqa: N802
        self._c._set(overflow_at=float(v), bins_overflow=True)

    @property
    def BinsUnderflowEnabled(self):  # noqa: N802
        return bool(self._c._get().get("bins_underflow"))

    @BinsUnderflowEnabled.setter
    def BinsUnderflowEnabled(self, v):  # noqa: N802
        self._c._set(bins_underflow=bool(v))

    @property
    def BinsUnderflowValue(self):  # noqa: N802
        return self._c._get().get("underflow_at", 0.0)

    @BinsUnderflowValue.setter
    def BinsUnderflowValue(self, v):  # noqa: N802
        self._c._set(underflow_at=float(v), bins_underflow=True)

    def FullCategoryCollection(self, Index=None):  # noqa: N802,N803
        """The chart's categories, filtered or not (Chart Filters): .Count, (i).Name, (i).IsFiltered."""
        col = _Categories(self._c)
        return col if Index is None else col.Item(Index)

    def CategoryCollection(self, Index=None):  # noqa: N802,N803
        """The categories the chart shows (those not filtered)."""
        shown = [c for c in _Categories(self._c) if not c.IsFiltered]
        if Index is None:
            return shown
        i = int(Index)
        if not 1 <= i <= len(shown):
            raise SubscriptOutOfRangeError("Category index out of range")
        return shown[i - 1]


class _Category:
    def __init__(self, chart: "Chart", index: int, name: str):
        self._c, self._i, self._name = chart, index, name  # 0-based

    def __repr__(self):
        return f"<Category {self._i + 1} {self._name!r}>"

    @property
    def Name(self):  # noqa: N802
        return self._name

    @property
    def IsFiltered(self):  # noqa: N802
        return self._i in self._c._get().get("hidden_cats", [])

    @IsFiltered.setter
    def IsFiltered(self, v):  # noqa: N802
        hidden = [k for k in self._c._get().get("hidden_cats", []) if k != self._i]
        if v:
            hidden.append(self._i)
        self._c._set(hidden_cats=sorted(hidden))


class _Categories:
    """ChartGroup.FullCategoryCollection: the categories of the first series' X values (or 1..N)."""

    def __init__(self, chart: "Chart"):
        self._c = chart

    def _names(self):
        from . import excel

        ss = self._c._get()["series"]
        if not ss:
            return []
        s0 = ss[0]
        try:
            if s0.get("x_values"):
                names = []
                for part in s0["x_values"].split(","):  # one range, or several areas
                    rng = excel.Range(part)
                    nr, nc = rng.Rows.Count, rng.Columns.Count
                    if nr > 1 and nc > 1:  # several category columns (treemap, sunburst): the innermost level
                        names += [str(rng.Cells(r, nc).Text) for r in range(1, nr + 1)]
                    else:
                        names += [str(rng.Cells(r, c).Text) for r in range(1, nr + 1) for c in range(1, nc + 1)]
                return names
            if s0.get("values"):
                return [str(i + 1) for i in range(excel.Range(s0["values"]).Count)]
        except Exception:  # noqa: BLE001
            pass
        return [str(i + 1) for i in range(s0.get("count", 0))]

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def __len__(self):
        return self.Count

    def __iter__(self):
        return iter([_Category(self._c, i, n) for i, n in enumerate(self._names())])

    def Item(self, Index):  # noqa: N802,N803
        names = self._names()
        if isinstance(Index, str):
            for i, n in enumerate(names):
                if n == Index:
                    return _Category(self._c, i, n)
            raise SubscriptOutOfRangeError(f"No category named {Index!r}")
        i = int(Index)
        if not 1 <= i <= len(names):
            raise SubscriptOutOfRangeError("Category index out of range")
        return _Category(self._c, i - 1, names[i - 1])

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self


class SeriesCollection:
    def __init__(self, chart: "Chart"):
        self._c = chart

    @property
    def Count(self):  # noqa: N802
        return len(self._c._get()["series"])

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        ss = self._c._get()["series"]
        if isinstance(Index, int):
            if not 1 <= Index <= len(ss):
                raise SubscriptOutOfRangeError("Series index out of range")
            return Series(self._c, Index - 1)
        for i, s in enumerate(ss):
            if s["name"] == Index:
                return Series(self._c, i)
        raise SubscriptOutOfRangeError(f"Series not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([Series(self._c, i) for i in range(self.Count)])

    def NewSeries(self):  # noqa: N802
        n = self.Count
        _rq("chart.series_set", sheet=self._c._sheet, name=self._c._name, index=-1, props={"name": f"Series{n + 1}"})
        return Series(self._c, n)

    def Add(self, Source, Rowcol=None, SeriesLabels=None, CategoryLabels=None, Replace=False):  # noqa: N802,N803
        n = self.Count
        _rq("chart.series_set", sheet=self._c._sheet, name=self._c._name, index=-1, props={"values": _addr(Source)})
        return Series(self._c, n)


class Chart:
    def __init__(self, sheet_id: int, name: str):
        self._sheet, self._name = sheet_id, name

    def __repr__(self):
        return f"<Chart {self._name}>"

    @property
    def Shapes(self):  # noqa: N802
        """The shapes drawn on the chart (lines, arrows, text boxes: a support level, a trade's mark): positions in
        points from the chart's top left. They move with the chart and go when it is deleted."""
        return _ChartShapes(self)

    @property
    def DisplayValueNotAvailableAsBlank(self):  # noqa: N802
        """#N/A in the data is drawn as an empty cell (as DisplayBlanksAs says: a gap by default); False draws the
        line through it. True in a new chart, as in Excel 16."""
        return bool(self._get().get("na_as_blank", True))

    @DisplayValueNotAvailableAsBlank.setter
    def DisplayValueNotAvailableAsBlank(self, v):  # noqa: N802
        self._set(na_as_blank=bool(v))

    def ExportAsFixedFormat(self, Type=0, Filename=None, Quality=None, IncludeDocProperties=None, IgnorePrintAreas=None,  # noqa: N802,N803
                            From=None, To=None, OpenAfterPublish=False, FixedFormatExtClassPtr=None):
        """The chart as it looks on the screen in a one-page PDF (Type xlTypePDF, 0); Filename gets ".pdf" if it has
        no extension."""
        import os
        import tempfile

        if int(Type) != 0:
            raise UnsupportedError("ExportAsFixedFormat: only xlTypePDF")
        if not Filename:
            raise ApplicationDefinedError("ExportAsFixedFormat needs a Filename")
        path = os.path.abspath(str(Filename))
        if not os.path.splitext(path)[1]:
            path += ".pdf"
        fd, png = tempfile.mkstemp(suffix=".png")
        os.close(fd)
        try:
            if not self.Export(png):
                raise ApplicationDefinedError("The chart couldn't be drawn (is it on the sheet shown?)")
            _png_to_pdf(png, path)
        finally:
            try:
                os.remove(png)
            except OSError:
                pass
        if OpenAfterPublish:
            from . import excel

            excel._open_on_desktop(path)

    def CopyPicture(self, Appearance=1, Format=-4147, Size=1):  # noqa: N802,N803
        """Copy the chart as it looks on the screen to the Clipboard, as a picture (to paste into other programs)."""
        _rq("ui.screenshot", path="", target="chart", address=self._name, sheet=self._sheet, include_headers=False,
            max_width=0, to_clipboard=True)

    def Export(self, Filename, FilterName="PNG", Interactive=False):  # noqa: N802,N803
        """Save the chart as it looks on the screen as a PNG file; True when it was written."""
        import os

        r = _rq("ui.screenshot", path=os.path.abspath(str(Filename)), target="chart", address=self._name, sheet=self._sheet,
                include_headers=False, max_width=0)
        return bool(r and r.get("ok"))

    @property
    def PivotLayout(self):  # noqa: N802
        """A PivotChart's PivotLayout (.PivotTable); None for an ordinary chart. SetSourceData(pt.TableRange1) makes a PivotChart."""
        return _PivotLayout(self) if self._get().get("pivot") else None

    @property
    def ShowAllFieldButtons(self):  # noqa: N802
        """A PivotChart's field buttons (PivotChart Analyze > Field Buttons); False for an ordinary chart."""
        pl = self.PivotLayout
        pt = pl.PivotTable if pl else None
        return bool(pt and pt.ShowAllFieldButtons)

    @ShowAllFieldButtons.setter
    def ShowAllFieldButtons(self, v):  # noqa: N802
        pl = self.PivotLayout
        pt = pl.PivotTable if pl else None
        if pt is None:
            raise ApplicationDefinedError("The chart is not a PivotChart")
        pt.ShowAllFieldButtons = bool(v)

    def _get(self) -> dict:
        return _rq("chart.get", sheet=self._sheet, name=self._name)

    def _set(self, **props) -> dict:
        d = _rq("chart.set", sheet=self._sheet, name=self._name, props=props)
        if d and "name" in props:
            self._name = d["name"]
        return d

    @property
    def Name(self):  # noqa: N802
        return self._name

    @property
    def ChartType(self):  # noqa: N802
        return _TYPE_TO_XL.get(self._get()["type"], 4)

    @ChartType.setter
    def ChartType(self, v):  # noqa: N802
        t = _XL_TO_TYPE.get(int(v))
        if t is None:
            raise UnsupportedError(f"Unsupported chart type: {v}")
        self._set(type=t)

    def ChartGroups(self, Index=None):  # noqa: N802,N803
        return ChartGroup(self)

    def ApplyDataLabels(self, Type=None, LegendKey=None, AutoText=None, HasLeaderLines=None, ShowSeriesName=None,  # noqa: N802,N803
                        ShowCategoryName=None, ShowValue=True, ShowPercentage=None, ShowBubbleSize=None, Separator=None):
        for i in range(len(self._get()["series"])):
            Series(self, i).ApplyDataLabels(Type, LegendKey, AutoText, HasLeaderLines, ShowSeriesName, ShowCategoryName, ShowValue, ShowPercentage,
                                            ShowBubbleSize, Separator)

    @property
    def PlotVisibleOnly(self):  # noqa: N802
        """True (Excel's default): rows and columns hidden or filtered out are left out of the chart."""
        return bool(self._get().get("plot_visible_only", True))

    @PlotVisibleOnly.setter
    def PlotVisibleOnly(self, v):  # noqa: N802
        self._set(plot_visible_only=bool(v))

    @property
    def DisplayBlanksAs(self):  # noqa: N802
        """Empty cells: xlNotPlotted (gaps, the default), xlZero, or xlInterpolated (the line joins over them)."""
        return int(self._get().get("display_blanks", 1))

    @DisplayBlanksAs.setter
    def DisplayBlanksAs(self, v):  # noqa: N802
        if int(v) not in (1, 2, 3):
            raise UnsupportedError(f"Unsupported DisplayBlanksAs: {v}")
        self._set(display_blanks=int(v))

    @property
    def DisplayNAAsBlank(self):  # noqa: N802
        """[YC] Hidden and Empty Cells > "Show #N/A as an empty cell" (True, the default): #N/A is plotted like an empty
        cell (DisplayBlanksAs); False: the line joins over it."""
        return bool(self._get().get("na_as_blank", True))

    @DisplayNAAsBlank.setter
    def DisplayNAAsBlank(self, v):  # noqa: N802
        self._set(na_as_blank=bool(v))

    @property
    def ChartColor(self):  # noqa: N802
        """Change Colors: 10-13 the colorful palettes, 14-19 the monochromatic ones."""
        return 10 + int(self._get().get("color_scheme", 0))

    @ChartColor.setter
    def ChartColor(self, v):  # noqa: N802
        if not 10 <= int(v) <= 19:
            raise UnsupportedError(f"Unsupported chart color: {v}")
        self._set(color_scheme=int(v) - 10)

    @property
    def ChartStyle(self):  # noqa: N802
        """Chart Design > Chart Styles: the chart type's set, numbered as Excel's (columns 201-214, bars 216-228,
        lines 227-241, scatter 240-254, pies 251-262, doughnuts 251-260, areas 276-286, the others 201-208); Style 1
        until a style is picked."""
        d = self._get()
        return int(d.get("style_base", 201)) + max(1, int(d.get("style", 0))) - 1

    @ChartStyle.setter
    def ChartStyle(self, v):  # noqa: N802
        d = self._get()
        base, count, v = int(d.get("style_base", 201)), int(d.get("style_count", 8)), int(v)
        n = v - base + 1 if base <= v < base + count else v if 1 <= v <= count else 0
        if not 1 <= n <= count:
            raise UnsupportedError(f"Unsupported chart style for this chart type: {v} (Style 1-{count} are {base}-{base + count - 1})")
        self._set(apply_style=n)

    def ApplyLayout(self, Layout, ChartType=None):  # noqa: N802,N803
        """Chart Design > Quick Layout: Layout 1-11 (which elements show and where); ChartType changes the type first."""
        n = int(Layout)
        if not 1 <= n <= 11:
            raise UnsupportedError(f"Unsupported layout: {Layout}")
        if ChartType is not None:
            self.ChartType = ChartType
        self._set(apply_layout=n)

    def SetSourceData(self, Source, PlotBy=None):  # noqa: N802,N803
        p = {"sheet": self._sheet, "name": self._name, "source": _addr(Source)}
        if PlotBy is not None:
            p["plot_by"] = int(PlotBy)
        _rq("chart.set_source", **p)

    @property
    def PlotBy(self):  # noqa: N802
        """xlRows (1) or xlColumns (2): whether the series come from the rows or the columns of the data range."""
        return 1 if self._get().get("plot_by_rows") else 2

    @PlotBy.setter
    def PlotBy(self, v):  # noqa: N802
        src = self._get().get("source", "")
        if not src:
            raise ApplicationDefinedError("This chart's data range isn't known: set it with SetSourceData")
        self.SetSourceData(src, PlotBy=int(v))

    def SetElement(self, Element):  # noqa: N802,N803
        """Chart Design > Add Chart Element: msoElementChartTitle..., msoElementLegend..., msoElementDataLabel...,
        msoElementPrimary(Category|Value)Axis..., ...AxisTitle..., ...GridLines... constants."""
        e = int(Element)
        if e in (0, 1, 2):
            self.HasTitle = e != 0
            if e:
                self._set(title_overlay=e == 1)
        elif 100 <= e <= 106:
            if e == 100:
                self.HasLegend = False
            else:
                self.Legend.Position = {101: -4152, 102: -4160, 103: -4131, 104: -4107, 105: -4152, 106: -4131}[e]
        elif e in (200, 201):
            self.ApplyDataLabels(ShowValue=e == 201)
        elif 202 <= e <= 210:
            pos = {202: -4108, 203: 3, 204: 4, 205: 2, 206: -4131, 207: -4152, 208: 0, 209: 1, 210: 5}[e]
            for i in range(len(self._get()["series"])):
                Series(self, i).DataLabels().Position = pos
        elif 600 <= e <= 604:
            for i in range(len(self._get()["series"])):
                s = Series(self, i)
                if e == 600:
                    s._set(trend=0)
                else:
                    s._set(trend={601: 1, 602: 2, 603: 1, 604: 6}[e], trend_forward=2.0 if e == 603 else 0.0, trend_period=2)
        elif 700 <= e <= 703:
            for i in range(len(self._get()["series"])):
                s = Series(self, i)
                if e == 700:
                    s._set(err=0)
                else:
                    s.ErrorBar(Type={701: 4, 702: 2, 703: -4155}[e])
        elif 300 <= e <= 303:
            self.Axes(1).HasTitle = e != 300
        elif 306 <= e <= 310:
            self.Axes(2).HasTitle = e != 306
        elif 328 <= e <= 331:
            self._set(cat_grid=e in (330, 331), cat_minor_grid=e in (329, 331))
        elif 332 <= e <= 335:
            self._set(val_grid=e in (334, 335), val_minor_grid=e in (333, 335))
        elif 348 <= e <= 351:
            self.Axes(1).Visible = e != 348
        elif 352 <= e <= 355:
            self.Axes(2).Visible = e != 352
        else:
            raise UnsupportedError(f"Chart element {e} isn't supported")

    def ClearToMatchStyle(self):  # noqa: N802
        """Format > Reset to Match Style for the whole chart: fills, lines, fonts and series colors back to automatic."""
        ChartArea(self).ClearFormats()
        for i in range(len(self._get()["series"])):
            Series(self, i).Color = 0

    @property
    def HasTitle(self):  # noqa: N802
        return self._get()["has_title"]

    @HasTitle.setter
    def HasTitle(self, v):  # noqa: N802
        self._set(has_title=bool(v))

    @property
    def ChartTitle(self):  # noqa: N802
        return _ChartTitle(self)

    @property
    def HasLegend(self):  # noqa: N802
        return self._get()["has_legend"]

    @HasLegend.setter
    def HasLegend(self, v):  # noqa: N802
        self._set(has_legend=bool(v))

    @property
    def Interactive(self):  # noqa: N802
        """[YC] Allow ImPlot pan and zoom (by default, like Excel, charts can only be selected and moved)."""
        return self._get()["interactive"]

    @Interactive.setter
    def Interactive(self, v):  # noqa: N802
        self._set(interactive=bool(v))

    @property
    def SeriesCollection(self):  # noqa: N802
        sc = SeriesCollection(self)

        class _SC(SeriesCollection):
            def __call__(self_inner, Index=None):  # noqa: N803
                return sc if Index is None else sc.Item(Index)

        out = _SC(self)
        return out

    def Axes(self, Type=None, AxisGroup=1):  # noqa: N802,N803
        """xlCategory or xlValue; Axes(xlValue, xlSecondary) is the secondary vertical axis (series on AxisGroup 2)."""
        return _Axis(self, 2 if Type is None else int(Type), int(AxisGroup or 1))

    # 3-D view (Format Chart Area > 3-D Rotation); reading gives the chart type's defaults until one is set
    def _view(key):  # noqa: N805
        def g(self):
            return self._get()[key]

        def s(self, v):
            self._set(**{key: float(v)})

        return property(g, s)

    Rotation = _view("rotation")
    Elevation = _view("elevation")
    Perspective = _view("perspective")
    DepthPercent = _view("depth_percent")
    del _view

    @property
    def RightAngleAxes(self):  # noqa: N802
        return self._get()["perspective"] == 0

    @RightAngleAxes.setter
    def RightAngleAxes(self, v):  # noqa: N802
        if v:
            self._set(perspective=0.0)

    def HasAxis(self, Index1=2, Index2=1):  # noqa: N802,N803
        """Whether the category (xlCategory) or value (xlValue) axis is shown (Index2 xlSecondary: the secondary
        vertical axis, shown when a series is on it); hide one with Axes(...).Delete()."""
        d = self._get()
        if int(Index1) == 2 and int(Index2) == 2:
            volume = d.get("type") in (48, 49) and len(d["series"]) >= 4  # a volume stock chart: the prices on it
            return bool(d.get("val2_visible", True)) and (volume or any(s.get("axis_group") == 1 for s in d["series"]))
        return bool(d[("val" if int(Index1) == 2 else "cat") + "_visible"])

    @property
    def ChartArea(self):  # noqa: N802
        return ChartArea(self)

    @property
    def PlotArea(self):  # noqa: N802
        return PlotArea(self)

    @property
    def Legend(self):  # noqa: N802
        return Legend(self)

    @property
    def Floor(self):  # noqa: N802
        """A 3-D chart's floor: Format.Fill, Interior, Select."""
        return _StockElem(self, "floor_fill", 0xF2F2F2, _ELEM_FLOOR)

    @property
    def Walls(self):  # noqa: N802
        """A 3-D chart's back and side walls: Format.Fill, Interior, Select."""
        return _StockElem(self, "wall_fill", 0xFFFFFF, _ELEM_WALLS)

    BackWall = Walls
    SideWall = Walls

    def Location(self, Where, Name=None):  # noqa: N802,N803
        """Move the chart: xlLocationAsNewSheet to a new chart sheet called Name, xlLocationAsObject onto the worksheet Name."""
        from . import excel

        where = int(Where)
        if where == 2:
            target = Name if hasattr(Name, "_id") else excel.Worksheets(Name)
            d = _rq("chart.move", sheet=self._sheet, name=self._name, to=target._id)
            self._sheet, self._name = d["sheet"], d["name"]
            return Chart(d["sheet"], d["name"])
        d = _rq("chart.move", sheet=self._sheet, name=self._name, to=0, new_name=str(Name or ""))
        self._sheet, self._name = d["sheet"], d["name"]
        return excel.ChartSheet(d["sheet"])

    def Select(self, Replace=True):  # noqa: N802,N803
        _select_elem(self, _ELEM_CHART_AREA)

    def Refresh(self):  # noqa: N802
        pass


class _PivotLayout:
    def __init__(self, chart):
        self._c = chart

    @property
    def PivotTable(self):  # noqa: N802
        name = self._c._get()["pivot"]
        if not name:
            return None
        from . import excel

        for ws in excel.Worksheets:
            for p in _rq("pivot.list", sheet=ws._id):
                if p["name"] == name:
                    return excel.PivotTable(ws._id, name)
        return None


class ChartObject:
    def __init__(self, sheet_id: int, name: str):
        self._sheet, self._name = sheet_id, name

    def __repr__(self):
        return f"<ChartObject {self._name}>"

    def _get(self) -> dict:
        return _rq("chart.get", sheet=self._sheet, name=self._name)

    def _set(self, **props) -> dict:
        d = _rq("chart.set", sheet=self._sheet, name=self._name, props=props)
        if d and "name" in props:
            self._name = d["name"]
        return d

    @property
    def Name(self):  # noqa: N802
        return self._name

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._set(name=str(v))

    @property
    def TopLeftCell(self):  # noqa: N802
        """The cell under the object's top-left corner."""
        return _cell_at(self._sheet, self.Left, self.Top)

    @property
    def BottomRightCell(self):  # noqa: N802
        """The cell under the object's bottom-right corner."""
        return _cell_at(self._sheet, self.Left + self.Width, self.Top + self.Height)

    def CopyPicture(self, Appearance=1, Format=-4147):  # noqa: N802,N803
        """Copies the chart as it looks on the screen to the Clipboard, as a picture."""
        self.Chart.CopyPicture(Appearance, Format)

    @property
    def Chart(self):  # noqa: N802
        return Chart(self._sheet, self._name)

    def _geom(name):  # noqa: N805
        def g(self):
            return self._get()[name]

        def s(self, v):
            self._set(**{name: float(v)})

        return property(g, s)

    Left = _geom("left")
    Top = _geom("top")
    Width = _geom("width")
    Height = _geom("height")
    del _geom

    @property
    def Placement(self):  # noqa: N802
        return self._get()["placement"]

    @Placement.setter
    def Placement(self, v):  # noqa: N802
        self._set(placement=int(v))

    @property
    def Floating(self):  # noqa: N802
        """[YC] Put the chart in its own dockable window (can be dragged into a container)."""
        return self._get()["floating"]

    @Floating.setter
    def Floating(self, v):  # noqa: N802
        self._set(floating=bool(v))

    @property
    def LockAspectRatio(self):  # noqa: N802
        """Format Chart Area > Size > Lock aspect ratio: corner handles keep the chart's shape (msoTrue -1 / msoFalse 0)."""
        return -1 if self._get().get("lock_aspect") else 0

    @LockAspectRatio.setter
    def LockAspectRatio(self, v):  # noqa: N802
        self._set(lock_aspect=bool(v))

    @property
    def PrintObject(self):  # noqa: N802
        """Format Chart Area > Properties > Print object."""
        return bool(self._get().get("print_object", True))

    @PrintObject.setter
    def PrintObject(self, v):  # noqa: N802
        self._set(print_object=bool(v))

    @property
    def Locked(self):  # noqa: N802
        """Format Chart Area > Properties > Locked: a protected sheet (without Edit objects) keeps it in place."""
        return bool(self._get().get("locked", True))

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        self._set(locked=bool(v))

    def Delete(self):  # noqa: N802
        _rq("chart.delete", sheet=self._sheet, name=self._name)

    def Copy(self):  # noqa: N802
        """Copy the chart (Ctrl+C on a selected chart); Worksheet.Paste pastes it as a new chart (on any sheet)."""
        self.Select()
        _rq("ui.command", id="ObjectCopy")

    def Cut(self):  # noqa: N802
        """Cut the chart (Ctrl+X): it goes at once; Worksheet.Paste puts it back."""
        self.Select()
        _rq("ui.command", id="ObjectCut")

    def Duplicate(self):  # noqa: N802
        """A copy of the chart a little down and right of it (Ctrl+D); returns the new ChartObject."""
        self.Select()
        _rq("ui.command", id="ObjectDuplicate")
        names = _rq("shape.selection")
        return ChartObject(self._sheet, names[0]) if names else None

    def Select(self, Replace=True):  # noqa: N802,N803
        """Select the chart on its sheet, as a click on it does (its Chart Design and Format tabs show)."""
        _rq("chart.select", sheet=self._sheet, name=self._name, elem=_ELEM_CHART_AREA, series=0, format=False)

    def Activate(self):  # noqa: N802
        self.Select()

    def BringToFront(self):  # noqa: N802
        lst = _list(self._sheet)
        self._set(z=max([c["z"] for c in lst["controls"]] + [c["z"] for c in lst["charts"]] + [0]) + 1)

    def SendToBack(self):  # noqa: N802
        lst = _list(self._sheet)
        self._set(z=min([c["z"] for c in lst["controls"]] + [c["z"] for c in lst["charts"]] + [0]) - 1)


class ChartObjects:
    def __init__(self, sheet_id: int):
        self._sheet = sheet_id

    def _names(self):
        return [c["name"] for c in _list(self._sheet)["charts"]]

    def Add(self, Left=0, Top=0, Width=360, Height=216, Name=None, ChartType=None, Source=None):  # noqa: N802,N803
        p = {"left": float(Left), "top": float(Top), "width": float(Width), "height": float(Height)}
        if Name:
            p["name"] = str(Name)
        if ChartType is not None:
            p["type"] = _XL_TO_TYPE.get(int(ChartType), 2)
        else:
            p["type"] = 2
        if Source is not None:
            p["source"] = _addr(Source)
        d = _rq("chart.add", sheet=self._sheet, **p)
        return ChartObject(self._sheet, d["name"])

    @property
    def Count(self):  # noqa: N802
        return len(self._names())

    def Item(self, Index):  # noqa: N802,N803
        names = self._names()
        if isinstance(Index, int):
            if not 1 <= Index <= len(names):
                raise SubscriptOutOfRangeError("ChartObjects index out of range")
            return ChartObject(self._sheet, names[Index - 1])
        for n in names:
            if n.lower() == str(Index).lower():
                return ChartObject(self._sheet, n)
        raise SubscriptOutOfRangeError(f"Chart not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([ChartObject(self._sheet, n) for n in self._names()])

    def __len__(self):
        return self.Count

    def Delete(self):  # noqa: N802
        for n in self._names():
            _rq("chart.delete", sheet=self._sheet, name=n)


class _Shape:
    """An item of the Shapes collection that wraps a chart (HasChart) or a control."""

    def __init__(self, obj):
        self._o = obj

    def __getattr__(self, name):
        return getattr(self._o, name)

    @property
    def HasChart(self):  # noqa: N802
        return isinstance(self._o, ChartObject)

    @property
    def Type(self):  # noqa: N802
        if isinstance(self._o, Shape):
            return self._o.Type
        return 3 if isinstance(self._o, ChartObject) else 12  # msoChart, msoOLEControlObject


# ShapeKind in the model <-> AutoShapeType
_KIND_PICTURE, _KIND_LINE, _KIND_ARROW, _KIND_TEXTBOX = 0, 5, 6, 8
_AUTOSHAPE_TO_KIND = {1: 1, 5: 2, 9: 3, 7: 4, 33: 7}
# every other preset shape of Excel's Shapes gallery (drawn from its msoAutoShapeType)
_AUTOSHAPE_PRESETS = frozenset(list(range(2, 61)) + list(range(61, 112)) + [113, 114, 115, 117, 118, 119, 121, 122, 123, 141, 142] +
                               list(range(144, 169)) + [179]) - {5, 9, 7, 33, 58, 112, 143}
# WordArt's 15 styles: letters' fill and outline (ARGB, 0 none), shadow, bold (Shapes.AddTextEffect)
_WORDART = [(0xFF000000, 0, True, False), (0xFF4472C4, 0, True, False), (0xFFED7D31, 0xFFC55A11, False, False), (0xFFFFFFFF, 0xFF4472C4, False, False),
            (0xFFFFC000, 0, False, True), (0xFF7F7F7F, 0, False, True), (0xFF5B9BD5, 0, True, True), (0xFFFFD966, 0xFFBF9000, False, True),
            (0xFFFFFFFF, 0xFF4472C4, True, False), (0xFF000000, 0xFFFFFFFF, True, True), (0xFF4472C4, 0xFFFFFFFF, True, True),
            (0xFFFFFFFF, 0xFFED7D31, True, True), (0xFFA5A5A5, 0, False, True), (0xFFFFFFFF, 0xFF5B9BD5, True, False), (0xFFD9D9D9, 0xFF404040, True, True)]
_KIND_TO_AUTOSHAPE = {v: k for k, v in _AUTOSHAPE_TO_KIND.items()}
_HALIGN = {-4131: 0, -4108: 1, -4152: 2, 1: 0, 2: 1, 3: 2}  # xlLeft / xlCenter / xlRight, msoAlignLeft / Center / Right
_VALIGN = {-4160: 0, -4108: 1, -4107: 2, 1: 0, 3: 1, 4: 2}  # xlTop / xlCenter / xlBottom, msoAnchorTop / Middle / Bottom


class _ColorFormat:
    def __init__(self, shape, key):
        self._s, self._k = shape, key

    @property
    def RGB(self):  # noqa: N802
        return _from_argb(self._s._get()[self._k])

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        self._s._set(**{self._k: _argb(v)})


class _FillFormat:
    def __init__(self, shape):
        self._s = shape

    @property
    def ForeColor(self):  # noqa: N802
        return _ColorFormat(self._s, "fill_color")

    @property
    def Visible(self):  # noqa: N802
        return -1 if self._s._get()["fill_visible"] else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._s._set(fill_visible=bool(v))

    def Solid(self):  # noqa: N802
        self._s._set(fill_visible=True)

    @property
    def Transparency(self):  # noqa: N802
        """0.0 (opaque) to 1.0 (clear)."""
        return self._s._get()["fill_transparency"]

    @Transparency.setter
    def Transparency(self, v):  # noqa: N802
        self._s._set(fill_transparency=float(v))


class _LineFormat:
    def __init__(self, shape):
        self._s = shape

    @property
    def ForeColor(self):  # noqa: N802
        return _ColorFormat(self._s, "line_color")

    @property
    def Visible(self):  # noqa: N802
        return -1 if self._s._get()["line_visible"] else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._s._set(line_visible=bool(v))

    @property
    def Weight(self):  # noqa: N802
        return self._s._get()["line_weight"]

    @Weight.setter
    def Weight(self, v):  # noqa: N802
        self._s._set(line_weight=float(v))

    @property
    def DashStyle(self):  # noqa: N802
        """msoLineSolid 1, msoLineSquareDot 2, msoLineRoundDot 3, msoLineDash 4, msoLineDashDot 5, msoLineLongDash 7."""
        return _DASH_FROM.get(self._s._get()["line_dash"], 1)

    @DashStyle.setter
    def DashStyle(self, v):  # noqa: N802
        self._s._set(line_dash=_DASH_TO.get(int(v), 0))

    @property
    def BeginArrowheadStyle(self):  # noqa: N802
        """msoArrowheadNone 1, msoArrowheadTriangle 2 (other heads are drawn as a triangle)."""
        return 2 if self._s._get()["begin_arrow"] else 1

    @BeginArrowheadStyle.setter
    def BeginArrowheadStyle(self, v):  # noqa: N802
        self._s._set(begin_arrow=1 if int(v) > 1 else 0)

    @property
    def EndArrowheadStyle(self):  # noqa: N802
        return 2 if self._s._get()["end_arrow"] else 1

    @EndArrowheadStyle.setter
    def EndArrowheadStyle(self, v):  # noqa: N802
        self._s._set(end_arrow=1 if int(v) > 1 else 0)


class _ShapeFont:
    def __init__(self, shape):
        self._s = shape

    def _prop(key, conv=lambda v: v, back=lambda v: v):  # noqa: N805
        def g(self):
            return back(self._s._get()[key])

        def s(self, v):
            self._s._set(**{key: conv(v)})

        return property(g, s)

    Name = _prop("font_name", str)
    Size = _prop("font_size", float)
    Bold = _prop("bold", bool)
    Italic = _prop("italic", bool)
    Color = _prop("font_color", _argb, _from_argb)
    del _prop

    @property
    def Fill(self):  # noqa: N802
        """TextFrame2 style: Font.Fill.ForeColor.RGB."""
        return _FontFill(self._s)


class _FontFill:
    def __init__(self, shape):
        self._s = shape

    @property
    def ForeColor(self):  # noqa: N802
        return _ColorFormat(self._s, "font_color")


class _TextRange:
    def __init__(self, shape):
        self._s = shape

    @property
    def Text(self):  # noqa: N802
        return self._s._get()["text"]

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._s._set(text=str(v))

    Caption = Text

    @property
    def Font(self):  # noqa: N802
        return _ShapeFont(self._s)

    def Characters(self, Start=None, Length=None):  # noqa: N802,N803
        return self


class _TextFrame:
    """Shape.TextFrame and TextFrame2 (the whole text; no per-character formatting)."""

    def __init__(self, shape):
        self._s = shape

    @property
    def TextRange(self):  # noqa: N802
        return _TextRange(self._s)

    def Characters(self, Start=None, Length=None):  # noqa: N802,N803
        return _TextRange(self._s)

    @property
    def HasText(self):  # noqa: N802
        return -1 if self._s._get()["text"] else 0

    @property
    def HorizontalAlignment(self):  # noqa: N802
        return (-4131, -4108, -4152)[self._s._get()["halign"]]

    @HorizontalAlignment.setter
    def HorizontalAlignment(self, v):  # noqa: N802
        self._s._set(halign=_HALIGN.get(int(v), 1))

    @property
    def VerticalAlignment(self):  # noqa: N802
        return (-4160, -4108, -4107)[self._s._get()["valign"]]

    @VerticalAlignment.setter
    def VerticalAlignment(self, v):  # noqa: N802
        self._s._set(valign=_VALIGN.get(int(v), 1))

    VerticalAnchor = VerticalAlignment

    def _margin(key):  # noqa: N805
        def g(self):
            return self._s._get()[key]

        def s(self, v):
            self._s._set(**{key: float(v)})

        return property(g, s)

    MarginLeft = _margin("margin_l")
    MarginTop = _margin("margin_t")
    MarginRight = _margin("margin_r")
    MarginBottom = _margin("margin_b")
    del _margin

    @property
    def WordWrap(self):  # noqa: N802
        return -1 if self._s._get()["wrap"] else 0

    @WordWrap.setter
    def WordWrap(self, v):  # noqa: N802
        self._s._set(wrap=bool(v))

    @property
    def AutoSize(self):  # noqa: N802
        """msoAutoSizeShapeToFitText (1): the shape's height follows its text when it is edited."""
        return 1 if self._s._get()["autofit"] else 0

    @AutoSize.setter
    def AutoSize(self, v):  # noqa: N802
        self._s._set(autofit=bool(v))


class _PictureFormat:
    """Shape.PictureFormat: Crop* in points of the picture as inserted."""

    def __init__(self, shape):
        self._s = shape

    def _crop(key):  # noqa: N805
        def g(self):
            return self._s._get()[key]

        def s(self, v):
            self._s._set(**{key: float(v)})

        return property(g, s)

    CropLeft = _crop("crop_l")
    CropTop = _crop("crop_t")
    CropRight = _crop("crop_r")
    CropBottom = _crop("crop_b")
    del _crop


# Line.DashStyle (msoLineDashStyle) <-> the model's dash index
_DASH_TO = {1: 0, 4: 1, 3: 2, 2: 3, 5: 4, 7: 5, -2: 0}
_DASH_FROM = {v: k for k, v in _DASH_TO.items() if k > 0}


class _FxColor:
    """ShadowFormat.ForeColor / GlowFormat.Color: RGB (Excel's BGR integer, as Range.Interior.Color)."""

    def __init__(self, shape, key):
        self._s, self._k = shape, key

    @property
    def RGB(self):  # noqa: N802
        c = self._s._get()["fx"][self._k] & 0xFFFFFF
        return ((c & 0xFF) << 16) | (c & 0xFF00) | (c >> 16)

    @RGB.setter
    def RGB(self, v):  # noqa: N802
        v = int(v)
        self._s._set(fx={self._k: ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF)})


class _FxFormat:
    """The base of Shape.Shadow / Glow / SoftEdge / Reflection / ThreeD: reads and writes the shape's effects."""

    _FIELDS = {}

    def __init__(self, shape):
        object.__setattr__(self, "_s", shape)

    def _fx(self):
        return self._s._get()["fx"]

    def _put(self, **kw):
        self._s._set(fx=kw)

    def __setattr__(self, name, value):
        if not hasattr(type(self), name):
            raise AttributeError(f"{type(self).__name__[1:]} has no property {name}")
        object.__setattr__(self, name, value)


def _fx_prop(key, get=float, put=float):
    return property(lambda self: get(self._fx()[key]), lambda self, v: self._put(**{key: put(v)}))


class _ShadowFormat(_FxFormat):
    """Shape.Shadow (Format Shape > Effects > Shadow): Visible, Style (msoShadowStyleOuterShadow 2, msoShadowStyleInnerShadow 1),
    ForeColor.RGB, Transparency (0-1), Size (percent), Blur, OffsetX, OffsetY (points)."""

    @property
    def Visible(self):  # noqa: N802
        return -1 if self._fx()["shadow"] else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        f = self._fx()
        self._put(shadow=(f["shadow"] or 1) if v else 0)

    @property
    def Style(self):  # noqa: N802
        return {1: 2, 2: 1, 3: 2}.get(self._fx()["shadow"], -2)

    @Style.setter
    def Style(self, v):  # noqa: N802
        if int(v) == -2:  # msoShadowStyleMixed: what a shape without a shadow reads; writing it back changes nothing
            return
        self._put(shadow=2 if int(v) == 1 else 1)

    @property
    def ForeColor(self):  # noqa: N802
        return _FxColor(self._s, "shadow_color")

    Transparency = _fx_prop("shadow_transparency")
    Size = _fx_prop("shadow_size")
    Blur = _fx_prop("shadow_blur")

    def _offsets(self):
        import math

        f = self._fx()
        a = math.radians(f["shadow_angle"])
        return math.cos(a) * f["shadow_distance"], math.sin(a) * f["shadow_distance"]

    def _set_offsets(self, x, y):
        import math

        self._put(shadow_angle=(math.degrees(math.atan2(y, x)) + 360) % 360, shadow_distance=math.hypot(x, y))

    @property
    def OffsetX(self):  # noqa: N802
        return self._offsets()[0]

    @OffsetX.setter
    def OffsetX(self, v):  # noqa: N802
        self._set_offsets(float(v), self._offsets()[1])

    @property
    def OffsetY(self):  # noqa: N802
        return self._offsets()[1]

    @OffsetY.setter
    def OffsetY(self, v):  # noqa: N802
        self._set_offsets(self._offsets()[0], float(v))


class _GlowFormat(_FxFormat):
    """Shape.Glow: Radius (points, 0 = none), Color.RGB, Transparency."""

    Radius = _fx_prop("glow_size")
    Transparency = _fx_prop("glow_transparency")

    @property
    def Color(self):  # noqa: N802
        return _FxColor(self._s, "glow_color")


class _SoftEdgeFormat(_FxFormat):
    """Shape.SoftEdge: Radius (points), Type (msoSoftEdgeTypeNone 0, msoSoftEdgeType1-6: 1, 2.5, 5, 10, 25, 50 pt)."""

    _TYPES = [0, 1, 2.5, 5, 10, 25, 50]
    Radius = _fx_prop("soft_edge")

    @property
    def Type(self):  # noqa: N802
        r = self._fx()["soft_edge"]
        return self._TYPES.index(r) if r in self._TYPES else -2

    @Type.setter
    def Type(self, v):  # noqa: N802
        self._put(soft_edge=float(self._TYPES[int(v)]))


class _ReflectionFormat(_FxFormat):
    """Shape.Reflection: Type (msoReflectionTypeNone 0, msoReflectionType1-9: tight / half / full, touching / 4 pt /
    8 pt offset), Transparency, Size (percent), Blur, Offset (points)."""

    _SIZES = [35, 55, 90]
    Transparency = _fx_prop("refl_transparency")
    Size = _fx_prop("refl_size")
    Blur = _fx_prop("refl_blur")
    Offset = _fx_prop("refl_distance")

    @property
    def Type(self):  # noqa: N802
        f = self._fx()
        if f["refl_size"] <= 0:
            return 0
        if f["refl_size"] in self._SIZES and f["refl_distance"] in (0, 4, 8):
            return 1 + self._SIZES.index(f["refl_size"]) + 3 * int(f["refl_distance"] // 4)
        return -2

    @Type.setter
    def Type(self, v):  # noqa: N802
        v = int(v)
        if v <= 0:
            self._put(refl_size=0.0)
        else:
            self._put(refl_size=float(self._SIZES[(v - 1) % 3]), refl_distance=float(((v - 1) // 3) * 4), refl_transparency=0.5)


# msoBevelType <-> the bevels of Format Shape > 3-D Format
_MSO_BEVEL = {0: 1, 1: 3, 2: 2, 3: 5, 4: 9, 5: 6, 6: 7, 7: 8, 8: 4, 9: 10, 10: 11, 11: 12, 12: 13}
_BEVEL_MSO = {v: k for k, v in _MSO_BEVEL.items()}


class _ThreeDFormat(_FxFormat):
    """Shape.ThreeD: BevelTopType / BevelBottomType (msoBevelNone 1, msoBevelCircle 3...), BevelTopInset / Depth,
    BevelBottomInset / Depth (points), RotationX, RotationY, RotationZ (degrees), FieldOfView, SetPresetCamera(n)
    (1-based in the 3-D Rotation gallery's order)."""

    BevelTopInset = _fx_prop("bevel_top_w")
    BevelTopDepth = _fx_prop("bevel_top_h")
    BevelBottomInset = _fx_prop("bevel_bottom_w")
    BevelBottomDepth = _fx_prop("bevel_bottom_h")
    RotationX = _fx_prop("rot_x")
    RotationY = _fx_prop("rot_y")
    RotationZ = _fx_prop("rot_z")
    FieldOfView = _fx_prop("perspective")

    @property
    def BevelTopType(self):  # noqa: N802
        return _MSO_BEVEL.get(self._fx()["bevel_top"], -2)

    @BevelTopType.setter
    def BevelTopType(self, v):  # noqa: N802
        self._put(bevel_top=_BEVEL_MSO.get(int(v), 0))

    @property
    def BevelBottomType(self):  # noqa: N802
        return _MSO_BEVEL.get(self._fx()["bevel_bottom"], -2)

    @BevelBottomType.setter
    def BevelBottomType(self, v):  # noqa: N802
        self._put(bevel_bottom=_BEVEL_MSO.get(int(v), 0))

    def SetPresetCamera(self, PresetCamera):  # noqa: N802,N803
        self._put(rotation_preset=int(PresetCamera))


class Shape:
    """A picture, an AutoShape, a line or a text box on a sheet (Insert -> Illustrations / Text Box)."""

    def __init__(self, sheet_id: int, name: str):
        self._sheet, self._name = sheet_id, name

    def __repr__(self):
        return f"<Shape {self._name}>"

    def _get(self) -> dict:
        return _rq("shape.get", sheet=self._sheet, name=self._name)

    def _set(self, **props) -> dict:
        d = _rq("shape.set", sheet=self._sheet, name=self._name, props=props)
        if d and "name" in props:
            self._name = d["name"]
        return d

    @property
    def Name(self):  # noqa: N802
        return self._name

    @Name.setter
    def Name(self, v):  # noqa: N802
        self._set(name=str(v))

    @property
    def ID(self):  # noqa: N802
        return self._get()["id"]

    @property
    def Type(self):  # noqa: N802
        k = self._get()["kind"]
        return {_KIND_PICTURE: 13, _KIND_LINE: 9, _KIND_ARROW: 9, _KIND_TEXTBOX: 17}.get(k, 1)

    @property
    def AutoShapeType(self):  # noqa: N802
        """The preset shape (msoShapeRectangle 1, msoShapeOval 9, msoShapeRightArrow 33, msoShape5pointStar 92...);
        a freeform is msoShapeNotPrimitive (138)."""
        d = self._get()
        if d.get("auto_type"):
            return d["auto_type"]
        return _KIND_TO_AUTOSHAPE.get(d["kind"], -2)  # msoShapeMixed

    @AutoShapeType.setter
    def AutoShapeType(self, v):  # noqa: N802
        v = int(v)
        if v in _AUTOSHAPE_TO_KIND:
            self._set(kind=_AUTOSHAPE_TO_KIND[v], auto_type=v)
        elif v in _AUTOSHAPE_PRESETS:
            self._set(kind=1, auto_type=v)
        else:
            raise ApplicationDefinedError(f"Unsupported AutoShapeType: {v}")

    # ---- Format Shape > Effects ----
    @property
    def Shadow(self):  # noqa: N802
        return _ShadowFormat(self)

    @property
    def Glow(self):  # noqa: N802
        return _GlowFormat(self)

    @property
    def SoftEdge(self):  # noqa: N802
        return _SoftEdgeFormat(self)

    @property
    def Reflection(self):  # noqa: N802
        return _ReflectionFormat(self)

    @property
    def ThreeD(self):  # noqa: N802
        return _ThreeDFormat(self)

    @property
    def ShapeStyle(self):  # noqa: N802
        """Shape Format > Shape Styles: msoShapeStylePreset1-42 (Theme Styles: 6 rows of 7 colors; rows 4-6 take the theme's
        subtle / moderate / intense effects); reading gives msoShapeStyleMixed (-2) unless it was set here."""
        return getattr(self, "_style", -2)

    @ShapeStyle.setter
    def ShapeStyle(self, v):  # noqa: N802
        _rq("layout.shape_style", sheet=self._sheet, id=self._get()["id"], style=int(v) - 1)
        self._style = int(v)

    @property
    def Adjustments(self):  # noqa: N802
        """The preset shape's adjustments (its yellow handles): Count, Item(n) (1-based, read / write), as Excel's
        Adjustments; the values are Excel's fractions (a rounded rectangle's corner 0.16667 of its shorter side)."""
        sh = self

        class _Adjustments:
            @property
            def Count(self):  # noqa: N802
                return len(sh._get().get("adj", []))

            def Item(self, Index):  # noqa: N802,N803
                a = sh._get().get("adj", [])
                if not 1 <= int(Index) <= len(a):
                    raise IndexError("Adjustments index out of range")
                return a[int(Index) - 1]

            def __getitem__(self, i):
                return self.Item(i)

            def __setitem__(self, i, v):
                a = list(sh._get().get("adj", []))
                if not 1 <= int(i) <= len(a):
                    raise IndexError("Adjustments index out of range")
                a[int(i) - 1] = float(v)
                sh._set(adj=a)

            def __call__(self, Index):  # noqa: N803
                return self.Item(Index)

            def __len__(self):
                return self.Count

        return _Adjustments()

    @property
    def Connector(self):  # noqa: N802
        """msoTrue (-1) for a connector (a line, an elbow or a curved connector)."""
        return -1 if self._get()["kind"] in (_KIND_LINE, _KIND_ARROW) else 0

    @property
    def ConnectorFormat(self):  # noqa: N802
        """.Type: msoConnectorStraight 1, msoConnectorElbow 2, msoConnectorCurve 3."""
        sh = self

        class _ConnectorFormat:
            @property
            def Type(self):  # noqa: N802
                return sh._get().get("connector") or 1

            @Type.setter
            def Type(self, v):  # noqa: N802
                sh._set(connector=int(v) if int(v) in (2, 3) else 0)

        return _ConnectorFormat()

    @property
    def HasChart(self):  # noqa: N802
        return False

    def _geom(name):  # noqa: N805
        def g(self):
            return self._get()[name]

        def s(self, v):
            self._set(**{name: float(v)})

        return property(g, s)

    Left = _geom("left")
    Top = _geom("top")
    Width = _geom("width")
    Height = _geom("height")
    del _geom

    @property
    def TopLeftCell(self):  # noqa: N802
        """The cell under the shape's top-left corner."""
        return _cell_at(self._sheet, self.Left, self.Top)

    @property
    def BottomRightCell(self):  # noqa: N802
        """The cell under the shape's bottom-right corner."""
        return _cell_at(self._sheet, self.Left + self.Width, self.Top + self.Height)

    def CopyPicture(self, Appearance=1, Format=-4147):  # noqa: N802,N803
        """Copies the cells the shape covers, as they look with it, to the Clipboard as a picture."""
        tl, br = self.TopLeftCell, self.BottomRightCell
        from . import excel

        excel.Worksheet(self._sheet).Range(tl, br).CopyPicture(Appearance, Format)

    def IncrementLeft(self, Increment):  # noqa: N802,N803
        self.Left = self.Left + float(Increment)

    def IncrementTop(self, Increment):  # noqa: N802,N803
        self.Top = self.Top + float(Increment)

    @property
    def Rotation(self):  # noqa: N802
        """Degrees clockwise (0-360)."""
        return self._get()["rotation"]

    @Rotation.setter
    def Rotation(self, v):  # noqa: N802
        self._set(rotation=float(v))

    def IncrementRotation(self, Increment):  # noqa: N802,N803
        self.Rotation = self.Rotation + float(Increment)

    @property
    def HorizontalFlip(self):  # noqa: N802
        return -1 if self._get()["flip_h"] else 0

    @property
    def VerticalFlip(self):  # noqa: N802
        return -1 if self._get()["flip_v"] else 0

    def Flip(self, FlipCmd):  # noqa: N802,N803
        """msoFlipHorizontal 0, msoFlipVertical 1."""
        d = self._get()
        if int(FlipCmd) == 0:
            self._set(flip_h=not d["flip_h"])
        else:
            self._set(flip_v=not d["flip_v"])

    @property
    def Locked(self):  # noqa: N802
        """On a protected sheet (without DrawingObjects=False) a locked shape can't be moved, sized or formatted."""
        return bool(self._get()["locked"])

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        self._set(locked=bool(v))

    @property
    def LockAspectRatio(self):  # noqa: N802
        """msoTrue (-1): sizing keeps the proportions (pictures by default)."""
        return -1 if self._get()["lock_aspect"] else 0

    @LockAspectRatio.setter
    def LockAspectRatio(self, v):  # noqa: N802
        self._set(lock_aspect=bool(v))

    @property
    def PrintObject(self):  # noqa: N802
        """Format Shape > Properties > "Print object" (Excel's DrawingObject.PrintObject): False leaves the shape out of
        printouts and PDFs."""
        return bool(self._get().get("print_object", True))

    @PrintObject.setter
    def PrintObject(self, v):  # noqa: N802
        self._set(print_object=bool(v))

    def ScaleHeight(self, Factor, RelativeToOriginalSize=False, Scale=0):  # noqa: N802,N803
        """Scale = msoScaleFromTopLeft 0, msoScaleFromMiddle 1, msoScaleFromBottomRight 2."""
        self._scale("height", "top", float(Factor), bool(RelativeToOriginalSize), int(Scale))

    def ScaleWidth(self, Factor, RelativeToOriginalSize=False, Scale=0):  # noqa: N802,N803
        self._scale("width", "left", float(Factor), bool(RelativeToOriginalSize), int(Scale))

    def _scale(self, size_key, pos_key, factor, original, scale):
        d = self._get()
        base = d["orig_h" if size_key == "height" else "orig_w"] if original and d["orig_w"] > 0 else d[size_key]
        new = max(1.0, base * factor)
        pos = d[pos_key]
        if scale == 1:
            pos -= (new - d[size_key]) / 2
        elif scale == 2:
            pos -= new - d[size_key]
        self._set(**{size_key: new, pos_key: max(0.0, pos)})

    @property
    def AlternativeText(self):  # noqa: N802
        return self._get()["alt_text"]

    @AlternativeText.setter
    def AlternativeText(self, v):  # noqa: N802
        self._set(alt_text=str(v))

    @property
    def PictureFormat(self):  # noqa: N802
        return _PictureFormat(self)

    def Select(self, Replace=True):  # noqa: N802,N803
        """Select the shape on its sheet, as a click does (Replace=False adds it to the selection)."""
        _rq("shape.select", sheet=self._sheet, name=self._name, replace=bool(Replace))

    def Copy(self):  # noqa: N802
        """Copy the shape; Worksheet.Paste pastes it (on any sheet)."""
        self.Select()
        _rq("ui.command", id="ObjectCopy")

    def Cut(self):  # noqa: N802
        self.Select()
        _rq("ui.command", id="ObjectCut")

    def Duplicate(self):  # noqa: N802
        """A copy a little down and right of the shape; returns it."""
        self.Select()
        _rq("ui.command", id="ObjectDuplicate")
        names = _rq("shape.selection")
        return Shape(self._sheet, names[0]) if names else None

    @property
    def GroupId(self):  # noqa: N802
        """[YC] the group the shape belongs to (0 = none); Shapes.Range([...]).Group() makes one."""
        return self._get()["group"]

    def Ungroup(self):  # noqa: N802
        g = self._get()["group"]
        if g:
            for s in Shapes(self._sheet)._all():
                if isinstance(s, Shape) and s._get()["group"] == g:
                    s._set(group=0)

    @property
    def Placement(self):  # noqa: N802
        return self._get()["placement"]

    @Placement.setter
    def Placement(self, v):  # noqa: N802
        self._set(placement=int(v))

    @property
    def Visible(self):  # noqa: N802
        return -1 if self._get()["visible"] else 0

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._set(visible=bool(v))

    @property
    def OnAction(self):  # noqa: N802
        """The macro run when the shape is clicked (outside Design Mode)."""
        return self._get()["on_action"]

    @OnAction.setter
    def OnAction(self, v):  # noqa: N802
        self._set(on_action=getattr(v, "__name__", str(v or "")))

    @property
    def Fill(self):  # noqa: N802
        return _FillFormat(self)

    @property
    def Line(self):  # noqa: N802
        return _LineFormat(self)

    @property
    def TextFrame(self):  # noqa: N802
        return _TextFrame(self)

    TextFrame2 = TextFrame

    @property
    def ZOrderPosition(self):  # noqa: N802
        lst = _list(self._sheet)
        zs = sorted([c["z"] for k in ("controls", "charts", "shapes") for c in lst.get(k, [])])
        return zs.index(self._get()["z"]) + 1

    def ZOrder(self, ZOrderCmd):  # noqa: N802,N803
        lst = _list(self._sheet)
        zs = [c["z"] for k in ("controls", "charts", "shapes") for c in lst.get(k, [])] + [0]
        z = self._get()["z"]
        cmd = int(ZOrderCmd)
        if cmd == 0:
            self._set(z=max(zs) + 1)
        elif cmd == 1:
            self._set(z=min(zs) - 1)
        elif cmd in (2, 3):
            # msoBringForward / msoSendBackward: past the next object above / below
            others = sorted(set(x for x in zs if (x > z if cmd == 2 else x < z)))
            if others:
                self._set(z=others[0] + 1 if cmd == 2 else others[-1] - 1)

    def Delete(self):  # noqa: N802
        _rq("shape.delete", sheet=self._sheet, name=self._name)


class ShapeRange:
    """Shapes.Range(["A", "B"]): several shapes at once (Group, Align, Distribute, Select)."""

    def __init__(self, sheet_id: int, names):
        self._sheet, self._names = sheet_id, list(names)

    @property
    def Count(self):  # noqa: N802
        return len(self._names)

    def Item(self, Index):  # noqa: N802,N803
        return Shape(self._sheet, self._names[int(Index) - 1]) if isinstance(Index, int) else Shape(self._sheet, str(Index))

    def __iter__(self):
        return iter([Shape(self._sheet, n) for n in self._names])

    def __len__(self):
        return len(self._names)

    def Select(self, Replace=True):  # noqa: N802,N803
        _rq("shape.select", sheet=self._sheet, names=self._names, replace=bool(Replace))

    def Group(self):  # noqa: N802
        """Join the shapes: they move and size together; returns the first one."""
        self.Select()
        _rq("ui.command", id="GroupObjects")
        return Shape(self._sheet, self._names[0])

    def Ungroup(self):  # noqa: N802
        self.Select()
        _rq("ui.command", id="UngroupObjects")
        return self

    def Align(self, AlignCmd, RelativeTo=False):  # noqa: N802,N803
        """msoAlignLefts 0, msoAlignCenters 1, msoAlignRights 2, msoAlignTops 3, msoAlignMiddles 4, msoAlignBottoms 5."""
        ids = ["ObjAlignLeft", "ObjAlignCenter", "ObjAlignRight", "ObjAlignTop", "ObjAlignMiddle", "ObjAlignBottom"]
        self.Select()
        _rq("ui.command", id=ids[int(AlignCmd)])

    def Distribute(self, DistributeCmd, RelativeTo=False):  # noqa: N802,N803
        """msoDistributeHorizontally 0, msoDistributeVertically 1."""
        self.Select()
        _rq("ui.command", id="ObjDistributeV" if int(DistributeCmd) else "ObjDistributeH")


class _ChartShape(Shape):
    """A shape of Chart.Shapes: Left and Top are from the chart's corner, as in Excel 16 (they stay when the chart
    moves; read from the sheet's corner they changed with it, while AddLine took them from the chart's: gaps_fuzz
    seed 17)."""

    def __init__(self, sheet_id: int, name: str, chart):
        super().__init__(sheet_id, name)
        self._chart = chart

    def _corner(self):
        d = self._chart._get()
        return float(d.get("left", 0.0)), float(d.get("top", 0.0))

    @property
    def Left(self):  # noqa: N802
        return self._get()["left"] - self._corner()[0]

    @Left.setter
    def Left(self, v):  # noqa: N802
        self._set(left=float(v) + self._corner()[0])

    @property
    def Top(self):  # noqa: N802
        return self._get()["top"] - self._corner()[1]

    @Top.setter
    def Top(self, v):  # noqa: N802
        self._set(top=float(v) + self._corner()[1])

    @property
    def TopLeftCell(self):  # noqa: N802
        d = self._get()
        return _cell_at(self._sheet, d["left"], d["top"])

    @property
    def BottomRightCell(self):  # noqa: N802
        d = self._get()
        return _cell_at(self._sheet, d["left"] + d["width"], d["top"] + d["height"])


class _ChartShapes:
    """Chart.Shapes: shapes on the chart's sheet that belong to the chart (Shape.on_chart), placed from its corner."""

    def __init__(self, chart):
        self._c = chart

    def _origin(self):
        d = self._c._get()
        return d, float(d.get("left", 0.0)), float(d.get("top", 0.0))

    def _mine(self):
        cid = self._c._get().get("id")
        lst = sorted(_list(self._c._sheet).get("shapes", []), key=lambda c: c["z"])
        return [_ChartShape(self._c._sheet, c["name"], self._c) for c in lst if c.get("on_chart") == cid]

    def _own(self, shape):
        shape._set(on_chart=int(self._c._get()["id"]))
        return _ChartShape(shape._sheet, shape.Name, self._c)

    def AddShape(self, Type, Left, Top, Width, Height):  # noqa: N802,N803
        _, x, y = self._origin()
        return self._own(Shapes(self._c._sheet).AddShape(Type, x + float(Left), y + float(Top), Width, Height))

    def AddTextbox(self, Orientation, Left, Top, Width, Height):  # noqa: N802,N803
        _, x, y = self._origin()
        return self._own(Shapes(self._c._sheet).AddTextbox(Orientation, x + float(Left), y + float(Top), Width, Height))

    AddLabel = AddTextbox

    def AddLine(self, BeginX, BeginY, EndX, EndY):  # noqa: N802,N803
        _, x, y = self._origin()
        return self._own(Shapes(self._c._sheet).AddLine(x + float(BeginX), y + float(BeginY), x + float(EndX), y + float(EndY)))

    @property
    def Count(self):  # noqa: N802
        return len(self._mine())

    def Item(self, Index):  # noqa: N802,N803
        mine = self._mine()
        if isinstance(Index, int):
            if not 1 <= Index <= len(mine):
                raise SubscriptOutOfRangeError("Shapes index out of range")
            return mine[Index - 1]
        for sh in mine:
            if sh.Name.lower() == str(Index).lower():
                return sh
        raise SubscriptOutOfRangeError(f"Shape not found: {Index}")

    def __call__(self, *args):
        return self.Item(*args) if args else self

    def __iter__(self):
        return iter(self._mine())

    def __len__(self):
        return self.Count


class Shapes:
    def __init__(self, sheet_id: int):
        self._sheet = sheet_id

    def _all(self):
        lst = _list(self._sheet)
        items = [(c["z"], (FormControl if c.get("form") else OLEObject)(self._sheet, c["name"])) for c in lst["controls"]]
        items += [(c["z"], ChartObject(self._sheet, c["name"])) for c in lst["charts"]]
        # a chart's own shapes (Chart.Shapes) are the chart's, not the sheet's (Excel 16): Worksheet.Shapes counted
        # them (gaps_fuzz seed 17)
        items += [(c["z"], Shape(self._sheet, c["name"])) for c in lst.get("shapes", []) if not c.get("on_chart")]
        return [o for _, o in sorted(items, key=lambda t: t[0])]

    def AddChart2(self, Style=-1, XlChartType=51, Left=None, Top=None, Width=None, Height=None, NewLayout=True):  # noqa: N802,N803
        co = ChartObjects(self._sheet).Add(Left or 0, Top or 0, Width or 360, Height or 216, ChartType=XlChartType)
        return _Shape(co)

    AddChart = AddChart2

    def _add(self, **p) -> Shape:
        d = _rq("shape.add", sheet=self._sheet, **p)
        return Shape(self._sheet, d["name"])

    def AddPicture(self, Filename, LinkToFile=False, SaveWithDocument=True, Left=0, Top=0, Width=-1, Height=-1):  # noqa: N802,N803
        """Insert a picture file (PNG, JPEG, BMP, GIF). Width / Height -1 = its own size. The picture is saved in the workbook."""
        with open(str(Filename), "rb") as f:
            data = f.read()
        return self._add(kind=_KIND_PICTURE, image=data, left=float(Left), top=float(Top), width=float(Width), height=float(Height))

    AddPicture2 = AddPicture

    def AddShape(self, Type, Left, Top, Width, Height):  # noqa: N802,N803
        """A preset shape of Excel's Shapes gallery by its msoAutoShapeType (msoShapeRectangle 1, msoShapeOval 9,
        msoShapeRightArrow 33, msoShapeFlowchartDecision 63, msoShape5pointStar 92, msoShapeCloud 179...)."""
        t = int(Type)
        kind = _AUTOSHAPE_TO_KIND.get(t)
        geom = dict(left=float(Left), top=float(Top), width=float(Width), height=float(Height))
        if kind is not None:
            return self._add(kind=kind, auto_type=t, **geom)
        if t not in _AUTOSHAPE_PRESETS:
            raise ApplicationDefinedError(f"Unsupported AutoShapeType: {Type}")
        return self._add(kind=1, auto_type=t, **geom)

    def AddCurve(self, SafeArrayOfPoints):  # noqa: N802,N803
        """A curve through the points [[x1, y1], [x2, y2], ...] (points, sheet coordinates), as Insert > Shapes > Curve."""
        return self._freeform(SafeArrayOfPoints, smooth=True)

    def AddPolyline(self, SafeArrayOfPoints):  # noqa: N802,N803
        """Lines through the points; back to the first point it is a closed, filled shape (Freeform: Shape)."""
        return self._freeform(SafeArrayOfPoints, smooth=False)

    def _freeform(self, pts, smooth):
        pts = [(float(p[0]), float(p[1])) for p in pts]
        if len(pts) < 2:
            raise ApplicationDefinedError("At least two points are needed")
        closed = len(pts) > 2 and pts[0] == pts[-1]
        x0, y0 = min(p[0] for p in pts), min(p[1] for p in pts)
        w, h = max(1.0, max(p[0] for p in pts) - x0), max(1.0, max(p[1] for p in pts) - y0)
        path = []
        for x, y in pts:
            path += [(x - x0) / w, (y - y0) / h]
        return self._add(kind=1, auto_type=138, path=path, path_closed=closed, path_smooth=bool(smooth), fill_visible=closed,
                         left=x0, top=y0, width=w, height=h)

    def AddTextEffect(self, PresetTextEffect, Text, FontName="", FontSize=54, FontBold=False, FontItalic=False, Left=0, Top=0):  # noqa: N802,N803
        """WordArt: a text box in one of Excel's 15 WordArt styles (msoTextEffect1 = 0 ... msoTextEffect15 = 14)."""
        fill, outline, shadow, bold = _WORDART[max(0, min(14, int(PresetTextEffect)))]
        return self._add(kind=_KIND_TEXTBOX, text=str(Text), font_name=str(FontName or ""), font_size=float(FontSize), bold=bool(FontBold) or bold,
                         italic=bool(FontItalic), font_color=fill, text_outline=outline, text_shadow=shadow, fill_visible=False, line_visible=False,
                         halign=1, wrap=False, autofit=True, left=float(Left), top=float(Top), width=max(36.0, len(str(Text)) * float(FontSize) * 0.55),
                         height=float(FontSize) * 1.25 + 7.2)

    def AddTextbox(self, Orientation, Left, Top, Width, Height):  # noqa: N802,N803
        return self._add(kind=_KIND_TEXTBOX, left=float(Left), top=float(Top), width=float(Width), height=float(Height))

    def AddLine(self, BeginX, BeginY, EndX, EndY):  # noqa: N802,N803
        bx, by, ex, ey = float(BeginX), float(BeginY), float(EndX), float(EndY)
        return self._add(kind=_KIND_LINE, left=min(bx, ex), top=min(by, ey), width=abs(ex - bx), height=abs(ey - by),
                         flip_h=ex < bx, flip_v=ey < by)

    def AddConnector(self, Type, BeginX, BeginY, EndX, EndY):  # noqa: N802,N803
        """msoConnectorStraight 1, msoConnectorElbow 2, msoConnectorCurve 3 from (BeginX, BeginY) to (EndX, EndY)."""
        sp = self.AddLine(BeginX, BeginY, EndX, EndY)
        if int(Type) in (2, 3):
            _rq("shape.set", sheet=self._sheet, name=sp.Name, props={"connector": int(Type)})
        return sp

    def AddFormControl(self, Type, Left, Top, Width, Height):  # noqa: N802,N803
        """A form control (xlButtonControl, xlCheckBox, xlDropDown, xlListBox, xlOptionButton, xlGroupBox, xlLabel,
        xlScrollBar, xlSpinner); set its OnAction and ControlFormat afterwards."""
        kind = _FORM_TYPES.get(int(Type))
        if kind is None:
            raise UnsupportedError(f"Unsupported form control type: {Type} (xlEditBox is not available in Excel worksheets either)")
        return _add_form(self._sheet, kind, Left, Top, Width, Height)

    @property
    def Count(self):  # noqa: N802
        return len(self._all())

    def Item(self, Index):  # noqa: N802,N803
        items = self._all()
        if isinstance(Index, int):
            if not 1 <= Index <= len(items):
                raise SubscriptOutOfRangeError("Shapes index out of range")
            return _Shape(items[Index - 1])
        for o in items:
            if o.Name.lower() == str(Index).lower():
                return _Shape(o)
        raise SubscriptOutOfRangeError(f"Shape not found: {Index}")

    def Range(self, Index):  # noqa: N802,N803
        """Several shapes by name or position: Shapes.Range(["Rectangle 1", "Oval 2"])."""
        idx = Index if isinstance(Index, (list, tuple)) else [Index]
        names = [self.Item(i).Name for i in idx]
        return ShapeRange(self._sheet, names)

    def SelectAll(self):  # noqa: N802
        ShapeRange(self._sheet, [o.Name for o in self._all()]).Select()

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([_Shape(o) for o in self._all()])

    def __len__(self):
        return self.Count


# ---------------------------------------------------------------------------
# custom panels [YC]
# ---------------------------------------------------------------------------
_TYPE_NAMES = {1: "label", 2: "button", 3: "textbox", 4: "combobox", 5: "checkbox", 6: "slider", 7: "chart", 8: "separator", 9: "group",
               10: "listbox", 11: "option", 12: "spin"}
_DOCK_NAMES = {1: "left", 2: "right", 3: "top", 4: "bottom", 5: "tab", 6: "float"}
_actions: dict = {}  # (panel, control) -> callable
_actions_lock = threading.Lock()


def _panels() -> list:
    return _rq("panel.list")


def _panel_def(name: str) -> dict:
    for p in _panels():
        if p["name"] == name:
            return p
    raise SubscriptOutOfRangeError(f"Panel not found: {name}")


class PanelControl:
    def __init__(self, panel: str, name: str):
        self._p, self._n = panel, name

    def __repr__(self):
        return f"<PanelControl {self._p}.{self._n}>"

    def _get(self) -> dict:
        for c in _panel_def(self._p)["controls"]:
            if c["name"] == self._n:
                return c
        raise SubscriptOutOfRangeError(f"Panel control not found: {self._n}")

    def _set(self, **props):
        _rq("panel.ctl_set", panel=self._p, name=self._n, props=props)

    @property
    def Name(self):  # noqa: N802
        return self._n

    @property
    def Type(self):  # noqa: N802
        return self._get()["type"]

    @property
    def Caption(self):  # noqa: N802
        return self._get()["caption"]

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        self._set(caption=str(v))

    @property
    def Value(self):  # noqa: N802
        c = self._get()
        if c["linked_cell"]:
            from . import excel

            lv = excel.Range(c["linked_cell"]).Value
            if lv is not None and lv != "":
                return lv
        v = c["value"]
        if c["type"] == "checkbox":
            return v.upper() in ("TRUE", "1")
        if c["type"] in ("slider", "spin"):
            try:
                return float(v) if c["type"] == "slider" else int(float(v))
            except ValueError:
                return c["min"]
        return v

    @Value.setter
    def Value(self, v):  # noqa: N802
        if isinstance(v, bool):
            v = "TRUE" if v else "FALSE"
        self._set(value=v if isinstance(v, str) else repr(v) if isinstance(v, float) else str(v))

    Text = Value

    @property
    def Enabled(self):  # noqa: N802
        return self._get()["enabled"]

    @Enabled.setter
    def Enabled(self, v):  # noqa: N802
        self._set(enabled=bool(v))

    @property
    def Visible(self):  # noqa: N802
        return self._get()["visible"]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._set(visible=bool(v))

    @property
    def LinkedCell(self):  # noqa: N802
        return self._get()["linked_cell"]

    @LinkedCell.setter
    def LinkedCell(self, v):  # noqa: N802
        self._set(linked_cell=_addr(v))

    @property
    def List(self):  # noqa: N802
        return list(self._get()["items"])

    @List.setter
    def List(self, v):  # noqa: N802
        self._set(items=[str(x) for x in v])

    @property
    def OnAction(self):  # noqa: N802
        with _actions_lock:
            fn = _actions.get((self._p, self._n))
        return fn if fn is not None else self._get()["on_action"]

    @OnAction.setter
    def OnAction(self, v):  # noqa: N802
        if callable(v):
            with _actions_lock:
                _actions[(self._p, self._n)] = v
            self._set(on_action="")
        else:
            with _actions_lock:
                _actions.pop((self._p, self._n), None)
            self._set(on_action=str(v or ""))

    # ---- [YC] styling: Excel RGB numbers (RGB(255, 0, 0)), None = the theme's ----
    @property
    def BackColor(self):  # noqa: N802
        v = self._get().get("back_color", -1)
        return None if v < 0 else v

    @BackColor.setter
    def BackColor(self, v):  # noqa: N802
        self._set(back_color=-1 if v is None else int(v))

    @property
    def ForeColor(self):  # noqa: N802
        v = self._get().get("fore_color", -1)
        return None if v < 0 else v

    @ForeColor.setter
    def ForeColor(self, v):  # noqa: N802
        self._set(fore_color=-1 if v is None else int(v))

    @property
    def FontSize(self):  # noqa: N802
        """Points (0 = the panel's size)."""
        return self._get().get("font_size", 0)

    @FontSize.setter
    def FontSize(self, v):  # noqa: N802
        self._set(font_size=float(v or 0))

    @property
    def Bold(self):  # noqa: N802
        return bool(self._get().get("bold", False))

    @Bold.setter
    def Bold(self, v):  # noqa: N802
        self._set(bold=bool(v))

    @property
    def NumberFormat(self):  # noqa: N802
        """A label's linked value shown with an Excel format code; "[Red]#,##0;[Green]-#,##0" colors it by sign."""
        return self._get().get("number_format", "")

    @NumberFormat.setter
    def NumberFormat(self, v):  # noqa: N802
        self._set(number_format=str(v or ""))

    @property
    def Height(self):  # noqa: N802
        """A button's height in points (0 = one text line)."""
        return self._get().get("height", 0)

    @Height.setter
    def Height(self, v):  # noqa: N802
        self._set(height=float(v or 0))

    @property
    def SmallChange(self):  # noqa: N802
        """A spin's or slider's step: one click of the arrows (a strike width of 100: SmallChange=100)."""
        return self._get().get("small_change", 1)

    @SmallChange.setter
    def SmallChange(self, v):  # noqa: N802
        self._set(small_change=float(v))

    def Click(self):  # noqa: N802
        """[YC] Click the button like the user (its OnAction runs in the host)."""
        _rq("panel.input", panel=self._p, control=self._n, action="click")


class PanelControls:
    def __init__(self, panel: "Panel"):
        self._panel = panel

    def _defs(self):
        return _panel_def(self._panel._name)["controls"]

    def Add(self, Type, Name=None, Caption="", LinkedCell=None, OnAction=None, List=None, ListFillRange=None,  # noqa: N802,N803
            Min=0, Max=100, Value=None, Chart=None, Enabled=True, Visible=True, **Props):
        """Add a control. Any other property of PanelControl can be given too and is set right away: ForeColor,
        BackColor, FontSize, Bold, NumberFormat, Height, SmallChange."""
        bad = [k for k in Props if not isinstance(getattr(PanelControl, k, None), property)]
        if bad:
            raise TypeError(f"PanelControls.Add: no property {', '.join(bad)} (PanelControl has ForeColor, BackColor, FontSize, Bold, "
                            "NumberFormat, Height, SmallChange)")
        t = _TYPE_NAMES.get(Type, str(Type).lower()) if not isinstance(Type, str) else Type.lower().removeprefix("yc")
        t = {"optiongroup": "option", "optionbutton": "option", "spinbutton": "spin"}.get(t, t)
        defs = self._defs()
        if Name is None:
            n = 1
            while any(c["name"] == f"{t}{n}" for c in defs):
                n += 1
            Name = f"{t}{n}"
        c = {"type": t, "name": str(Name), "caption": str(Caption or ""), "linked_cell": _addr(LinkedCell), "list_fill_range": _addr(ListFillRange),
             "items": [str(x) for x in (List or [])], "min": float(Min), "max": float(Max), "enabled": bool(Enabled), "visible": bool(Visible),
             "chart": Chart.Name if hasattr(Chart, "Name") else str(Chart or ""), "on_action": "" if callable(OnAction) else str(OnAction or "")}
        if Value is not None:
            c["value"] = ("TRUE" if Value else "FALSE") if isinstance(Value, bool) else str(Value)
        defs = [d for d in defs if d["name"] != c["name"]] + [c]
        _rq("panel.set", panel={"name": self._panel._name, "controls": defs, "keep_values": True})
        if callable(OnAction):
            with _actions_lock:
                _actions[(self._panel._name, c["name"])] = OnAction
        ctl = PanelControl(self._panel._name, c["name"])
        for k, v in Props.items():
            setattr(ctl, k, v)
        return ctl

    @property
    def Count(self):  # noqa: N802
        return len(self._defs())

    def Item(self, Index):  # noqa: N802,N803
        defs = self._defs()
        if isinstance(Index, int):
            if not 1 <= Index <= len(defs):
                raise SubscriptOutOfRangeError("Panel control index out of range")
            return PanelControl(self._panel._name, defs[Index - 1]["name"])
        for d in defs:
            if d["name"] == Index:
                return PanelControl(self._panel._name, d["name"])
        raise SubscriptOutOfRangeError(f"Panel control not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([PanelControl(self._panel._name, d["name"]) for d in self._defs()])

    def __len__(self):
        return self.Count

    def Remove(self, Name):  # noqa: N802,N803
        defs = [d for d in self._defs() if d["name"] != Name]
        _rq("panel.set", panel={"name": self._panel._name, "controls": defs})

    def Clear(self):  # noqa: N802
        _rq("panel.set", panel={"name": self._panel._name, "controls": []})


class Panel:
    def __init__(self, name: str):
        self._name = name

    def __repr__(self):
        return f"<Panel {self._name}>"

    def __getattr__(self, name):
        # panel.Stop -> control
        if name.startswith("_"):
            raise AttributeError(name)
        for c in _panel_def(self._name)["controls"]:
            if c["name"] == name:
                return PanelControl(self._name, name)
        raise AttributeError(f"The panel has no member {name}")

    def _set(self, **props):
        _rq("panel.set", panel={"name": self._name, **props})

    @property
    def Name(self):  # noqa: N802
        return self._name

    @property
    def Caption(self):  # noqa: N802
        return _panel_def(self._name)["caption"]

    @property
    def Locked(self):  # noqa: N802
        """[YC] True: the user can't move, undock, resize or close the panel (its controls still work; Python still
        docks and sizes it). Saved with the workbook's layout. ThisWorkbook.Protect(Windows=True) locks every window."""
        return bool(_rq("ui.lock", kind="panel", name=self._name))

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        _rq("ui.lock", kind="panel", name=self._name, locked=bool(v))

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        self._set(caption=str(v))

    @property
    def Visible(self):  # noqa: N802
        return _panel_def(self._name)["visible"]

    @Visible.setter
    def Visible(self, v):  # noqa: N802
        self._set(visible=bool(v))

    def Show(self):  # noqa: N802
        self.Visible = True

    def Hide(self):  # noqa: N802
        self.Visible = False

    @property
    def BackColor(self):  # noqa: N802
        """[YC] The panel's background (an Excel RGB number; None = the theme's). Frames, buttons and popups follow its
        lightness, so a dark panel sits well beside a dark chart."""
        v = _panel_def(self._name).get("back_color", -1)
        return None if v is None or v < 0 else v

    @BackColor.setter
    def BackColor(self, v):  # noqa: N802
        self._set(back_color=-1 if v is None else int(v))

    @property
    def ForeColor(self):  # noqa: N802
        """[YC] The panel's text color (None = follows BackColor / the theme)."""
        v = _panel_def(self._name).get("fore_color", -1)
        return None if v is None or v < 0 else v

    @ForeColor.setter
    def ForeColor(self, v):  # noqa: N802
        self._set(fore_color=-1 if v is None else int(v))

    @property
    def Width(self):  # noqa: N802
        return _panel_def(self._name)["width"]

    @Width.setter
    def Width(self, v):  # noqa: N802
        self._set(width=float(v))

    @property
    def Controls(self):  # noqa: N802
        return PanelControls(self)

    def Rebuild(self, KeepValues=True):  # noqa: N802,N803
        """Clear the controls before rebuilding; with KeepValues=True controls with the same name keep their old values (applied in Controls.Add)."""
        if not KeepValues:
            _rq("panel.set", panel={"name": self._name, "controls": [], "keep_values": False})
        else:
            # keep the definitions so a later Controls.Add can reuse the values by name
            pass
        return self

    def Delete(self):  # noqa: N802
        with _actions_lock:
            for k in [k for k in _actions if k[0] == self._name]:
                _actions.pop(k, None)
        _rq("panel.delete", name=self._name)


class _Panels:
    def Add(self, Name, Caption=None, Dock=2, Width=320, Height=240, Visible=True):  # noqa: N802,N803
        dock = _DOCK_NAMES.get(Dock, str(Dock).lower()) if not isinstance(Dock, str) else Dock.lower()
        p = {"name": str(Name), "caption": str(Caption if Caption is not None else Name), "dock": dock, "width": float(Width),
             "height": float(Height), "visible": bool(Visible)}
        _rq("panel.set", panel=p)
        return Panel(str(Name))

    @property
    def Count(self):  # noqa: N802
        return len(_panels())

    def Item(self, Index):  # noqa: N802,N803
        ps = _panels()
        if isinstance(Index, int):
            if not 1 <= Index <= len(ps):
                raise SubscriptOutOfRangeError("Panels index out of range")
            return Panel(ps[Index - 1]["name"])
        for p in ps:
            if p["name"] == Index:
                return Panel(p["name"])
        raise SubscriptOutOfRangeError(f"Panel not found: {Index}")

    def Exists(self, Name) -> bool:  # noqa: N802,N803
        """[YC] Whether a panel of that name exists."""
        return any(p["name"] == str(Name) for p in _panels())

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([Panel(p["name"]) for p in _panels()])

    def __len__(self):
        return self.Count

    def __contains__(self, name):
        return any(p["name"] == name for p in _panels())


Panels = _Panels()


def dispatch_panel_event(p: dict) -> None:
    """Panel control event (called by the host): callable -> OnAction string -> <panel>_<control>_<event>() in main.py."""
    from . import host

    full = p.get("name", "")
    panel, _, ctl = full.partition(".")
    event = p.get("event", "Click")
    from . import forms

    if forms.dispatch(panel, ctl, event):  # a shown UserForm handles its own controls
        return
    with _actions_lock:
        fn = _actions.get((panel, ctl))
    where = f"{panel}.{ctl}"
    if fn is None and p.get("on_action"):
        try:
            fn = host._resolve_proc(p["on_action"])
            where = p["on_action"]
        except Exception as e:  # noqa: BLE001
            host._report(p["on_action"], e)
            return
    if fn is None:
        main = host._modules.get("main")
        fn = getattr(main, f"{panel}_{ctl}_{event}", None) if main is not None else None
        if fn is None:
            return
    ctrl = PanelControl(panel, ctl)
    try:
        params = [x for x in inspect.signature(fn).parameters.values() if x.kind in (x.POSITIONAL_ONLY, x.POSITIONAL_OR_KEYWORD)]
    except (TypeError, ValueError):
        params = []
    if params:
        host._call(where, fn, ctrl)
    else:
        host._call(where, fn)


# ---------------------------------------------------------------------------
# containers and layout [YC]
# ---------------------------------------------------------------------------
_POS_NAMES = {1: "left", 2: "right", 3: "top", 4: "bottom", 5: "tab"}


def _item_spec(item) -> dict:
    from . import excel

    if isinstance(item, excel.Worksheet):
        return {"kind": "sheet", "name": item.Name}
    if isinstance(item, Panel):
        return {"kind": "panel", "name": item.Name}
    if isinstance(item, ChartObject):
        return {"kind": "chart", "name": item.Name}
    if isinstance(item, Container):
        return {"kind": "container", "name": item.Name}
    from . import plot

    if isinstance(item, plot.Figure):  # by key when it has one (the place survives in layouts)
        return {"kind": "figure", "name": item.key or str(item.id)}
    if isinstance(item, str):
        return {"kind": "sheet", "name": item}
    raise ApplicationDefinedError(f"Can't dock: {item!r}")


class Container:
    def __init__(self, name: str):
        self.Name = name

    def __repr__(self):
        return f"<Container {self.Name}>"

    def Dock(self, Item, Position=5, Ratio=0.3):  # noqa: N802,N803
        """Dock a sheet, panel, chart, container or yc.plot figure into this container."""
        pos = _POS_NAMES.get(Position, str(Position).lower()) if not isinstance(Position, str) else Position.lower()
        _rq("container.dock", name=self.Name, item=_item_spec(Item), position=pos, ratio=float(Ratio))

    def Delete(self):  # noqa: N802
        _rq("container.delete", name=self.Name)

    @property
    def Locked(self):  # noqa: N802
        """[YC] True: the user can't move, resize or close the container, undock what is in it or move its splitters.
        Saved with the workbook's layout."""
        return bool(_rq("ui.lock", kind="container", name=self.Name))

    @Locked.setter
    def Locked(self, v):  # noqa: N802
        _rq("ui.lock", kind="container", name=self.Name, locked=bool(v))


class _Containers:
    def Add(self, Name=None, Caption=None, Left=None, Top=None, Width=640, Height=420, Floating=True):  # noqa: N802,N803
        p = {"name": str(Name or Caption or ""), "width": float(Width), "height": float(Height)}
        if Left is not None and Top is not None:
            p["left"], p["top"] = float(Left), float(Top)
        d = _rq("container.add", **p)
        return Container(d["name"])

    @property
    def Count(self):  # noqa: N802
        return len(_rq("container.list"))

    def Item(self, Index):  # noqa: N802,N803
        cs = _rq("container.list")
        if isinstance(Index, int):
            if not 1 <= Index <= len(cs):
                raise SubscriptOutOfRangeError("Containers index out of range")
            return Container(cs[Index - 1]["name"])
        for c in cs:
            if c["name"] == Index:
                return Container(c["name"])
        raise SubscriptOutOfRangeError(f"Container not found: {Index}")

    def __call__(self, *args, **kwargs):
        # like Excel: Coll(Index) is Coll.Item(Index), Coll() is the collection itself
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([Container(c["name"]) for c in _rq("container.list")])

    def __len__(self):
        return self.Count


Containers = _Containers()


def Float(item, Left=None, Top=None, Width=None, Height=None):  # noqa: N802,N803
    """[YC] Turn sheets, panels and charts into floating windows."""
    p = {"item": _item_spec(item)}
    for k, v in (("left", Left), ("top", Top), ("width", Width), ("height", Height)):
        if v is not None:
            p[k] = float(v)
    _rq("window.float", **p)


class _Layout:
    """Named layouts: stored in the workbook's Store ("yc.layout.<name>") and saved with the file."""

    _PREFIX = "yc.layout."

    def Save(self, Name):  # noqa: N802,N803
        from . import excel

        excel.ThisWorkbook.Store[self._PREFIX + str(Name)] = _rq("layout.capture")

    def Restore(self, Name, MainWindow=False):  # noqa: N802,N803
        from . import excel

        js = excel.ThisWorkbook.Store.get(self._PREFIX + str(Name))
        if js is None:
            raise SubscriptOutOfRangeError(f"Layout not found: {Name}")
        _rq("layout.apply", json=js, main_window=bool(MainWindow))

    def Delete(self, Name):  # noqa: N802,N803
        from . import excel

        del excel.ThisWorkbook.Store[self._PREFIX + str(Name)]

    def Reset(self):  # noqa: N802
        """View > Reset Layout: the windows back to their default places."""
        _rq("layout.reset")

    @property
    def Names(self):  # noqa: N802
        from . import excel

        return [k[len(self._PREFIX):] for k in excel.ThisWorkbook.Store.keys() if k.startswith(self._PREFIX)]


Layout = _Layout()


def design_mode() -> bool:
    return bool(_rq("ui.design_mode_get")["on"])


def set_design_mode(on: bool) -> None:
    _rq("ui.design_mode_set", on=bool(on))


def _cell_at(sheet, left, top):
    """The cell under a point of the sheet (points from its top left)."""
    from . import excel

    r, c = _rq("sheet.cell_at", sheet=sheet, left=float(left), top=float(top))
    return excel.Range._from(sheet, [(r, c, r, c)])


def _png_to_pdf(png_path, pdf_path):
    """A one-page PDF holding a PNG picture at its size (72 dpi), written with the standard library only."""
    import struct
    import zlib

    with open(png_path, "rb") as f:
        data = f.read()
    pos, idat, w, h, ctype, depth = 8, b"", 0, 0, 2, 8
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + n
    if depth != 8 or ctype not in (2, 6):
        raise ValueError("an 8-bit RGB or RGBA PNG")
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        ft = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ft == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(bytes(line))
        prev = line
    rgb = b"".join(r if bpp == 3 else bytes(v for k, v in enumerate(r) if k % 4 != 3) for r in rows)
    img = zlib.compress(rgb, 9)
    content = f"q {w} 0 0 {h} 0 0 cm /Im0 Do Q".encode()
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {w} {h}] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>".encode(),
        f"<< /Type /XObject /Subtype /Image /Width {w} /Height {h} /ColorSpace /DeviceRGB /BitsPerComponent 8 "
        f"/Filter /FlateDecode /Length {len(img)} >>\nstream\n".encode() + img + b"\nendstream",
        f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"\nendstream",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for k, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += f"{k} 0 obj\n".encode() + o + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n".encode()
    for off in offsets:
        out += f"{off:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    with open(pdf_path, "wb") as f:
        f.write(bytes(out))

