# SPDX-License-Identifier: Apache-2.0
"""UserForms: dialogs with MSForms-style controls, written in Python.

    from yc.excel import *

    class OrderForm(UserForm):
        def UserForm_Initialize(self):
            self.Caption = "New order"
            self.Width, self.Height = 240, 150
            self.Controls.Add("Forms.Label.1", "lblQty").Caption = "Quantity"
            self.Controls.Add("Forms.TextBox.1", "txtQty")
            self.Controls.Add("Forms.CommandButton.1", "btnOK").Caption = "OK"

        def btnOK_Click(self):
            Range("A1").Value = self.txtQty.Value
            self.Hide()

    OrderForm().Show()          # modal: returns after Hide / Unload

Events are methods named like Excel's: <Control>_Click, <Control>_Change, UserForm_Initialize (when created),
UserForm_Activate (on Show), UserForm_QueryClose(CloseMode) (the close box; return True to keep the form open)
and UserForm_Terminate (on Unload). Controls are placed at Left / Top / Width / Height (points); controls added
without a position are stacked down the form. YCGrid draws the form as a panel that isn't saved with the workbook.
"""
from __future__ import annotations

import itertools
import threading
from typing import Any, Optional

from ._conn import YcError
from .errors import SubscriptOutOfRangeError, UnsupportedError

_rq: Any = None  # set by _bind() (the connection's request function, shared with excel.py)

# MSForms ProgID -> the panel control type YCGrid draws
_PROG = {"commandbutton": "button", "label": "label", "textbox": "textbox", "combobox": "combobox", "listbox": "listbox",
         "checkbox": "checkbox", "optionbutton": "optionbutton", "togglebutton": "togglebutton", "spinbutton": "spin",
         "scrollbar": "slider", "frame": "frame"}
_DEFAULT_SIZE = {"button": (72, 24), "label": (72, 18), "textbox": (120, 18), "combobox": (120, 18), "listbox": (120, 72),
                 "checkbox": (108, 18), "optionbutton": (108, 18), "togglebutton": (72, 24), "spin": (60, 18), "slider": (120, 18),
                 "frame": (160, 90)}
_NAME_PREFIX = {"button": "CommandButton", "label": "Label", "textbox": "TextBox", "combobox": "ComboBox", "listbox": "ListBox",
                "checkbox": "CheckBox", "optionbutton": "OptionButton", "togglebutton": "ToggleButton", "spin": "SpinButton",
                "slider": "ScrollBar", "frame": "Frame"}
_BOOLS = ("checkbox", "optionbutton", "togglebutton")

_forms: dict = {}  # panel name -> UserForm (shown forms receive their events)
_lock = threading.Lock()
_seq = itertools.count(1)


def _bind():
    global _rq
    if _rq is None:
        from . import excel

        _rq = excel._rq


def _prog_type(prog: str) -> str:
    t = str(prog).strip().lower()
    if t.startswith("forms."):
        t = t[6:]
    t = t.split(".")[0]
    if t not in _PROG:
        raise UnsupportedError(f"Unsupported UserForm control: {prog}")
    return _PROG[t]


class FormControl:
    """A control on a UserForm (MSForms names: Caption, Value, Text, List, ListIndex, Left, Top...)."""

    def __init__(self, form: "UserForm", spec: dict):
        self.__dict__["_f"] = form
        self.__dict__["_s"] = spec

    def __repr__(self):
        return f"<{_NAME_PREFIX.get(self._s['type'], 'Control')} {self._s['name']}>"

    def __setattr__(self, name, value):
        if name.startswith("_") or hasattr(type(self), name):
            object.__setattr__(self, name, value)
        else:
            raise AttributeError(f"The control has no property {name}")

    def _live(self) -> dict:
        """The control as YCGrid has it now (the user may have typed or picked something)."""
        if self._f._shown:
            try:
                for p in _rq("panel.list"):
                    if p["name"] == self._f._panel:
                        for c in p["controls"]:
                            if c["name"] == self._s["name"]:
                                self._s["value"] = c["value"]
                                return self._s
            except YcError:
                pass
        return self._s

    def _put(self, **kw):
        self._s.update(kw)
        if self._f._shown:
            _rq("panel.ctl_set", panel=self._f._panel, name=self._s["name"], props=kw)

    @property
    def Name(self):  # noqa: N802
        return self._s["name"]

    def _prop(key, conv=str):  # noqa: N805
        def g(self):
            return self._s.get(key)

        def s(self, v):
            self._put(**{key: conv(v)})

        return property(g, s)

    Caption = _prop("caption")
    Left = _prop("left", float)
    Top = _prop("top", float)
    Width = _prop("width", float)
    Height = _prop("height", float)
    Enabled = _prop("enabled", bool)
    Visible = _prop("visible", bool)
    Min = _prop("min", float)
    Max = _prop("max", float)
    GroupName = _prop("group")
    del _prop

    @property
    def Value(self):  # noqa: N802
        d = self._live()
        v = d.get("value", "")
        if d["type"] in _BOOLS:
            return str(v).lower() in ("true", "1", "-1")
        if d["type"] in ("spin", "slider"):
            try:
                return int(float(v))
            except (TypeError, ValueError):
                return int(d.get("min", 0))
        return v

    @Value.setter
    def Value(self, v):  # noqa: N802
        t = self._s["type"]
        if t in _BOOLS:
            v = "True" if v in (True, 1, -1) or str(v).lower() == "true" else "False"
            if t == "optionbutton" and v == "True":
                # the other option buttons of the group turn off
                for c in self._f._specs:
                    if c is not self._s and c["type"] == "optionbutton" and c.get("group", "") == self._s.get("group", ""):
                        FormControl(self._f, c)._put(value="False")
        elif t in ("spin", "slider"):
            v = str(int(v))
        else:
            v = "" if v is None else str(v)
        self._put(value=v)

    @property
    def Text(self):  # noqa: N802
        return str(self._live().get("value", ""))

    @Text.setter
    def Text(self, v):  # noqa: N802
        self._put(value=str(v))

    # ---- lists ----
    @property
    def List(self):  # noqa: N802
        return list(self._s.get("items", []))

    @List.setter
    def List(self, v):  # noqa: N802
        self._put(items=[str(x) for x in v])

    @property
    def ListCount(self):  # noqa: N802
        return len(self._s.get("items", []))

    @property
    def ListIndex(self):  # noqa: N802
        """0-based like MSForms; -1 when nothing is chosen."""
        v = self._live().get("value", "")
        items = self._s.get("items", [])
        return items.index(v) if v in items else -1

    @ListIndex.setter
    def ListIndex(self, i):  # noqa: N802
        items = self._s.get("items", [])
        i = int(i)
        self._put(value=items[i] if 0 <= i < len(items) else "")

    def AddItem(self, Item, Index=None):  # noqa: N802,N803
        items = list(self._s.get("items", []))
        if Index is None:
            items.append(str(Item))
        else:
            items.insert(int(Index), str(Item))
        self._put(items=items)

    def RemoveItem(self, Index):  # noqa: N802,N803
        items = list(self._s.get("items", []))
        del items[int(Index)]
        self._put(items=items)

    def Clear(self):  # noqa: N802
        self._put(items=[], value="")

    def SetFocus(self):  # noqa: N802
        pass


class _Controls:
    def __init__(self, form: "UserForm"):
        self._f = form

    def Add(self, bstrProgID, Name=None, Visible=True):  # noqa: N802,N803
        t = _prog_type(bstrProgID)
        f = self._f
        if Name is None:
            n = 1
            while any(c["name"] == f"{_NAME_PREFIX[t]}{n}" for c in f._specs):
                n += 1
            Name = f"{_NAME_PREFIX[t]}{n}"
        if any(c["name"].lower() == str(Name).lower() for c in f._specs):
            raise YcError(f"A control named {Name} already exists", 1004, "E_ARG")
        w, h = _DEFAULT_SIZE[t]
        # stacked down the form until the caller sets Left / Top
        top = 6.0
        for c in f._specs:
            top = max(top, c["top"] + c["height"] + 6)
        spec = {"type": t, "name": str(Name), "caption": str(Name) if t in ("button", "checkbox", "optionbutton", "togglebutton", "frame") else "",
                "left": 6.0, "top": top, "width": float(w), "height": float(h), "value": "False" if t in _BOOLS else "",
                "items": [], "min": 0.0, "max": 100.0, "enabled": True, "visible": bool(Visible), "group": ""}
        f._specs.append(spec)
        if f._shown:
            f._push()
        return FormControl(f, spec)

    @property
    def Count(self):  # noqa: N802
        return len(self._f._specs)

    def __len__(self):
        return self.Count

    def Item(self, Index):  # noqa: N802,N803
        specs = self._f._specs
        if isinstance(Index, int):
            if not 0 <= Index < len(specs):
                raise SubscriptOutOfRangeError("Control index out of range")
            return FormControl(self._f, specs[Index])  # 0-based like MSForms
        for c in specs:
            if c["name"].lower() == str(Index).lower():
                return FormControl(self._f, c)
        raise SubscriptOutOfRangeError(f"Control not found: {Index}")

    def __call__(self, *args, **kwargs):
        return self.Item(*args, **kwargs) if args or kwargs else self

    def __iter__(self):
        return iter([FormControl(self._f, c) for c in self._f._specs])

    def Remove(self, Index):  # noqa: N802,N803
        c = self.Item(Index)
        self._f._specs.remove(c._s)
        if self._f._shown:
            self._f._push()


class UserForm:
    """Subclass it, add controls in UserForm_Initialize (or __init__) and call Show()."""

    def __init__(self, Caption: Optional[str] = None, Width: float = 240, Height: float = 180):  # noqa: N803
        _bind()
        d = self.__dict__
        d["_panel"] = f"UserForm{next(_seq)}_{type(self).__name__}"
        d["_specs"] = []
        d["_shown"] = False
        d["_modal_open"] = False
        d["_caption"] = type(self).__name__ if Caption is None else str(Caption)
        d["_w"], d["_h"] = float(Width), float(Height)
        init = getattr(self, "UserForm_Initialize", None)
        if callable(init):
            init()

    def __getattr__(self, name):
        # Me.TextBox1: a control by name
        for c in self.__dict__.get("_specs", []):
            if c["name"].lower() == name.lower():
                return FormControl(self, c)
        raise AttributeError(name)

    @property
    def Controls(self):  # noqa: N802
        return _Controls(self)

    @property
    def Caption(self):  # noqa: N802
        return self._caption

    @Caption.setter
    def Caption(self, v):  # noqa: N802
        self.__dict__["_caption"] = str(v)
        if self._shown:
            self._push()

    @property
    def Width(self):  # noqa: N802
        return self._w

    @Width.setter
    def Width(self, v):  # noqa: N802
        self.__dict__["_w"] = float(v)

    @property
    def Height(self):  # noqa: N802
        return self._h

    @Height.setter
    def Height(self, v):  # noqa: N802
        self.__dict__["_h"] = float(v)

    @property
    def Visible(self):  # noqa: N802
        return self._shown

    def _push(self, modal: bool = False):
        _rq("panel.set", panel={"name": self._panel, "caption": self._caption, "dock": "float", "form": True, "modal": modal,
                                "width": self._w, "height": self._h + 28, "visible": True,
                                "controls": [dict(c) for c in self._specs], "keep_values": False})

    def Show(self, Modal: int = 1):  # noqa: N803
        """Show the form. Modal (vbModal = 1, the default): wait until it is hidden or unloaded, handling its
        events meanwhile; vbModeless (0): return at once."""
        modal = bool(Modal)
        with _lock:
            _forms[self._panel] = self
        self.__dict__["_shown"] = True
        self._push(modal)
        act = getattr(self, "UserForm_Activate", None)
        if callable(act):
            act()
        if not modal:
            return
        from . import host

        if host._conn_obj is not None:
            host.pump_while(lambda: self._shown)
        else:
            # a script outside the workbook's host: no events arrive here, so wait until the user closes the form
            import time

            while self._shown:
                time.sleep(0.1)
                try:
                    if not any(p["name"] == self._panel and p["visible"] for p in _rq("panel.list")):
                        self.__dict__["_shown"] = False
                except YcError:
                    self.__dict__["_shown"] = False

    def Hide(self):  # noqa: N802
        if self._shown:
            self._sync_values()
            self.__dict__["_shown"] = False
            try:
                _rq("panel.delete", name=self._panel)
            except YcError:
                pass

    def Unload(self):  # noqa: N802
        self.Hide()
        with _lock:
            _forms.pop(self._panel, None)
        term = getattr(self, "UserForm_Terminate", None)
        if callable(term):
            term()

    def _sync_values(self):
        try:
            for p in _rq("panel.list"):
                if p["name"] == self._panel:
                    by = {c["name"]: c for c in p["controls"]}
                    for c in self._specs:
                        if c["name"] in by:
                            c["value"] = by[c["name"]]["value"]
        except YcError:
            pass

    def _event(self, ctl: str, event: str):
        from . import host

        if not ctl:  # the form's close box
            if event == "QueryClose":
                qc = getattr(self, "UserForm_QueryClose", None)
                cancel = bool(host._call("UserForm_QueryClose", qc, 0)) if callable(qc) else False
                if not cancel:
                    self.Unload()
            return
        fn = getattr(type(self), f"{ctl}_{event}", None)
        if fn is not None:
            host._call(f"{type(self).__name__}.{ctl}_{event}", fn, self)


def dispatch(panel: str, ctl: str, event: str) -> bool:
    """A panel event that belongs to a shown UserForm (called by ui.dispatch_panel_event)."""
    with _lock:
        f = _forms.get(panel)
    if f is None:
        return False
    f._event(ctl, event)
    return True


def Unload(form: UserForm):  # noqa: N802
    """Unload Me / Unload UserForm1."""
    form.Unload()


vbModal, vbModeless = 1, 0
