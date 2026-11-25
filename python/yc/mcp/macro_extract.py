# SPDX-License-Identifier: Apache-2.0
"""yc_extract_macros: extract macro code and the controls on sheets from Excel files (pure Python, no Excel needed).

- .xlsm / .xlsb / .xlam: the macro project .bin under xl/ in the zip (an OLE compound file)
- .xls: the whole file is an OLE compound file (the macro project is one of its storages)
- Macro source code is stored compressed in each module stream (the dir stream records module names and offsets)
- Controls: <control> in the sheet XML (ActiveX and form controls), ctrlProps, form buttons in VML
"""
from __future__ import annotations

import os
import re
import struct
import zipfile
from typing import Optional

# ---------------------------------------------------------------------------
# OLE compound file (CFB)
# ---------------------------------------------------------------------------
_FREE, _END = 0xFFFFFFFF, 0xFFFFFFFE


class Cfb:
    def __init__(self, data: bytes):
        if data[:8] != bytes.fromhex("D0CF11E0A1B11AE1"):
            raise ValueError("Not an OLE compound file")
        self.d = data
        self.ss = 1 << struct.unpack_from("<H", data, 0x1E)[0]
        self.mss = 1 << struct.unpack_from("<H", data, 0x20)[0]
        n_fat = struct.unpack_from("<I", data, 0x2C)[0]
        first_dir = struct.unpack_from("<I", data, 0x30)[0]
        self.cutoff = struct.unpack_from("<I", data, 0x38)[0]
        first_mfat, n_mfat = struct.unpack_from("<II", data, 0x3C)
        first_difat, n_difat = struct.unpack_from("<II", data, 0x44)
        difat = list(struct.unpack_from("<109I", data, 0x4C))
        sec = first_difat
        for _ in range(n_difat):
            if sec in (_FREE, _END):
                break
            vals = struct.unpack_from(f"<{self.ss // 4}I", data, self._off(sec))
            difat.extend(vals[:-1])
            sec = vals[-1]
        fat_secs = [s for s in difat if s not in (_FREE, _END)][:n_fat]
        self.fat = []
        for s in fat_secs:
            self.fat.extend(struct.unpack_from(f"<{self.ss // 4}I", data, self._off(s)))
        self.mfat = []
        if n_mfat:
            raw = self._chain(first_mfat)
            self.mfat = list(struct.unpack(f"<{len(raw) // 4}I", raw[: len(raw) // 4 * 4]))
        dir_raw = self._chain(first_dir)
        self.entries = []
        for i in range(len(dir_raw) // 128):
            e = dir_raw[i * 128:(i + 1) * 128]
            nlen = struct.unpack_from("<H", e, 64)[0]
            name = e[: max(0, nlen - 2)].decode("utf-16-le", "replace")
            typ = e[66]
            left, right, child = struct.unpack_from("<III", e, 68)
            start = struct.unpack_from("<I", e, 116)[0]
            size = struct.unpack_from("<Q", e, 120)[0] if self.ss == 4096 else struct.unpack_from("<I", e, 120)[0]
            self.entries.append({"name": name, "type": typ, "left": left, "right": right, "child": child, "start": start, "size": size})
        root = self.entries[0]
        self.ministream = self._chain(root["start"])[: root["size"]] if root["start"] not in (_FREE, _END) else b""
        self.paths: dict = {}
        self._walk(root["child"], "")

    def _off(self, sec: int) -> int:
        return (sec + 1) * self.ss

    def _chain(self, start: int) -> bytes:
        out = bytearray()
        sec, n = start, 0
        while sec not in (_FREE, _END) and sec < len(self.fat) + 1 and n < 1_000_000:
            out += self.d[self._off(sec):self._off(sec) + self.ss]
            sec = self.fat[sec] if sec < len(self.fat) else _END
            n += 1
        return bytes(out)

    def _walk(self, idx: int, prefix: str) -> None:
        stack = [idx]
        seen = set()
        while stack:
            i = stack.pop()
            if i in (_FREE, _END) or i >= len(self.entries) or i in seen:
                continue
            seen.add(i)
            e = self.entries[i]
            path = prefix + e["name"]
            self.paths[path.lower()] = i
            stack.append(e["left"])
            stack.append(e["right"])
            if e["type"] == 1:
                self._walk(e["child"], path + "/")

    def read(self, path: str) -> Optional[bytes]:
        i = self.paths.get(path.lower())
        if i is None:
            return None
        e = self.entries[i]
        if e["size"] < self.cutoff:
            out = bytearray()
            sec, n = e["start"], 0
            while sec not in (_FREE, _END) and sec < len(self.mfat) and n < 1_000_000:
                out += self.ministream[sec * self.mss:(sec + 1) * self.mss]
                sec = self.mfat[sec]
                n += 1
            return bytes(out[: e["size"]])
        return self._chain(e["start"])[: e["size"]]


# ---------------------------------------------------------------------------
# decompression of macro project streams
# ---------------------------------------------------------------------------
def decompress(data: bytes) -> bytes:
    if not data or data[0] != 1:
        raise ValueError("Not compressed macro project data")
    out = bytearray()
    pos = 1
    while pos + 2 <= len(data):
        header = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        size = (header & 0x0FFF) + 3
        compressed = (header >> 15) & 1
        end = min(len(data), pos + size - 2)
        start = len(out)
        if not compressed:
            out += data[pos:pos + 4096]
            pos += 4096
            continue
        while pos < end:
            flags = data[pos]
            pos += 1
            for bit in range(8):
                if pos >= end:
                    break
                if not (flags >> bit) & 1:
                    out.append(data[pos])
                    pos += 1
                else:
                    token = struct.unpack_from("<H", data, pos)[0]
                    pos += 2
                    diff = len(out) - start
                    bits = max((diff - 1).bit_length(), 4)
                    length = (token & (0xFFFF >> bits)) + 3
                    offset = (token >> (16 - bits)) + 1
                    for _ in range(length):
                        out.append(out[-offset])
    return bytes(out)


def _codec(cp: int) -> str:
    return {65001: "utf-8", 1200: "utf-16-le"}.get(cp, f"cp{cp}")


def _parse_dir(raw: bytes):
    pos = 0
    codepage = 1252
    modules = []
    cur: dict = {}
    while pos + 6 <= len(raw):
        rid, size = struct.unpack_from("<HI", raw, pos)
        pos += 6
        if rid == 0x0009:  # PROJECTVERSION: the size field is always 4, but the data is 6 bytes
            pos += 6
            continue
        data = raw[pos:pos + size]
        pos += size
        if rid == 0x0003:
            codepage = struct.unpack_from("<H", data)[0]
        elif rid == 0x0019:
            cur = {"name": data.decode(_codec(codepage), "replace")}
        elif rid == 0x0047:
            cur["name_u"] = data.decode("utf-16-le", "replace")
        elif rid == 0x001A:
            cur["stream"] = data.decode(_codec(codepage), "replace")
        elif rid == 0x0032:
            cur["stream_u"] = data.decode("utf-16-le", "replace")
        elif rid == 0x0031:
            cur["offset"] = struct.unpack_from("<I", data)[0]
        elif rid == 0x0021:
            cur["type"] = "standard"
        elif rid == 0x0022:
            cur["type"] = "document_or_class"
        elif rid == 0x002B:
            if cur:
                modules.append(cur)
            cur = {}
    return codepage, modules


def _project_kinds(text: str) -> dict:
    kinds = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        name = v.strip().split("/")[0].strip('"')
        if k == "Module":
            kinds[name] = "standard"
        elif k == "Class":
            kinds[name] = "class"
        elif k == "Document":
            kinds[name] = "document"
        elif k == "BaseClass":
            kinds[name] = "userform"
    return kinds


def _find_project(cfb: Cfb):
    """Returns (project storage path, code storage path), both ending in / (the root is ""); None when not found.
    Detected by structure: the code storage has a dir stream, the project storage (its parent) has a PROJECT stream."""
    for p in sorted(cfb.paths):
        if not p.endswith("dir") or (len(p) > 3 and p[-4] != "/"):
            continue
        code_dir = p[:-3]
        parent = code_dir[:-1].rpartition("/")[0]
        base = parent + "/" if parent else ""
        if code_dir and (base + "project") in cfb.paths:
            return base, code_dir
    return None


def _read_macros(cfb: Cfb):
    found = _find_project(cfb)
    if found is None:
        return None
    base, code_dir = found
    raw = cfb.read(code_dir + "dir")
    if raw is None:
        return None
    codepage, mods = _parse_dir(decompress(raw))
    proj = cfb.read(base + "PROJECT")
    kinds = _project_kinds(proj.decode(_codec(codepage), "replace")) if proj else {}
    out = []
    for m in mods:
        name = m.get("name_u") or m.get("name", "")
        stream = m.get("stream_u") or m.get("stream") or name
        data = cfb.read(code_dir + stream)
        code = ""
        if data is not None:
            try:
                code = decompress(data[m.get("offset", 0):]).decode(_codec(codepage), "replace")
            except (ValueError, IndexError, LookupError):
                code = "(couldn't decompress)"
        code = "\n".join(ln for ln in code.replace("\r\n", "\n").split("\n") if not ln.startswith("Attribute VB_"))
        out.append({"name": name, "kind": kinds.get(name, m.get("type", "standard")), "code": code.strip("\n")})
    return out


# ---------------------------------------------------------------------------
# sheets and controls (OOXML)
# ---------------------------------------------------------------------------
_CLSID = {"{D7053240-CE69-11CD-A777-00DD01143C57}": "CommandButton", "{8BD21D30-EC42-11CE-9E0D-00AA006002F3}": "ComboBox",
          "{8BD21D40-EC42-11CE-9E0D-00AA006002F3}": "CheckBox", "{8BD21D50-EC42-11CE-9E0D-00AA006002F3}": "OptionButton",
          "{8BD21D10-EC42-11CE-9E0D-00AA006002F3}": "TextBox", "{8BD21D20-EC42-11CE-9E0D-00AA006002F3}": "ListBox",
          "{978C9E23-D4B0-11CE-BF2D-00AA003F40D0}": "Label", "{8BD21D60-EC42-11CE-9E0D-00AA006002F3}": "ToggleButton",
          "{79176FB0-B7F2-11CE-97EF-00AA006D2776}": "SpinButton", "{DFD181E0-5E2F-11CE-A449-00AA004A803D}": "ScrollBar",
          "{4C599241-6926-101B-9992-00000B65C6F9}": "Image"}


def _rels(z: zipfile.ZipFile, part: str) -> dict:
    d, f = os.path.split(part)
    rp = f"{d}/_rels/{f}.rels"
    if rp not in z.namelist():
        return {}
    xml = z.read(rp).decode("utf-8", "replace")
    out = {}
    for m in re.finditer(r"<Relationship\b([^>]*)/?>", xml):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        tgt = attrs.get("Target", "")
        if not tgt.startswith("/"):
            tgt = os.path.normpath(os.path.join(d, tgt)).replace("\\", "/")
        else:
            tgt = tgt.lstrip("/")
        out[attrs.get("Id")] = tgt
    return out


def _attrs(s: str) -> dict:
    return {k.split(":")[-1]: v for k, v in re.findall(r'([\w:]+)="([^"]*)"', s)}


def _sheets(z: zipfile.ZipFile) -> list:
    if "xl/workbook.xml" not in z.namelist():
        return []
    wb = z.read("xl/workbook.xml").decode("utf-8", "replace")
    rels = _rels(z, "xl/workbook.xml")
    out = []
    for m in re.finditer(r"<sheet\b([^>]*)/>", wb):
        a = _attrs(m.group(1))
        part = rels.get(a.get("id"), "")
        code = ""
        if part in z.namelist():
            mm = re.search(r'<sheetPr\b[^>]*codeName="([^"]*)"', z.read(part).decode("utf-8", "replace"))
            code = mm.group(1) if mm else ""
        out.append({"name": a.get("name"), "part": part, "code_name": code})
    return out


def _controls(z: zipfile.ZipFile, sheet: dict) -> list:
    part = sheet["part"]
    if part not in z.namelist():
        return []
    xml = z.read(part).decode("utf-8", "replace")
    rels = _rels(z, part)
    out = []
    for m in re.finditer(r"<control\b([^>]*?)(?:/>|>(.*?)</control>)", xml, re.S):
        a = _attrs(m.group(1))
        body = m.group(2) or ""
        c = {"name": a.get("name"), "shape_id": a.get("shapeId")}
        pr = re.search(r"<controlPr\b([^>]*)>", body)
        if pr:
            pa = _attrs(pr.group(1))
            for k in ("linkedCell", "listFillRange", "macro", "altText"):
                if k in pa:
                    c[k] = pa[k]
        fr = re.search(r"<(?:\w+:)?from>.*?<(?:\w+:)?col>(\d+)</.*?<(?:\w+:)?row>(\d+)</", body, re.S)
        to = re.search(r"<(?:\w+:)?to>.*?<(?:\w+:)?col>(\d+)</.*?<(?:\w+:)?row>(\d+)</", body, re.S)
        if fr and to:
            c["anchor"] = {"from": {"col": int(fr.group(1)), "row": int(fr.group(2))}, "to": {"col": int(to.group(1)), "row": int(to.group(2))}}
        tgt = rels.get(a.get("id"), "")
        if "activeX" in tgt and tgt in z.namelist():
            ax = z.read(tgt).decode("utf-8", "replace")
            cm = re.search(r'classid="(\{[^}]+\})"', ax)
            c["type"] = "ActiveX " + _CLSID.get(cm.group(1).upper() if cm else "", cm.group(1) if cm else "?")
        elif "ctrlProp" in tgt and tgt in z.namelist():
            fm = re.search(r"<formControlPr\b([^>]*)", z.read(tgt).decode("utf-8", "replace"))
            cp = _attrs(fm.group(1)) if fm else {}
            c["type"] = "Form " + cp.get("objectType", "?")
            for k in ("fmlaLink", "fmlaRange", "fmlaMacro"):
                if k in cp:
                    c[k] = cp[k]
        out.append(c)
    # form buttons in VML (legacy): caption and OnAction
    for rid, tgt in rels.items():
        if tgt.endswith(".vml") and tgt in z.namelist():
            vml = z.read(tgt).decode("utf-8", "replace")
            for sm in re.finditer(r"<v:shape\b([^>]*)>(.*?)</v:shape>", vml, re.S):
                sa = _attrs(sm.group(1))
                inner = sm.group(2)
                ot = re.search(r'ObjectType="(\w+)"', inner)
                if not ot or ot.group(1) in ("Note",):
                    continue
                caption = re.sub(r"<[^>]+>", "", (re.search(r"<v:textbox\b.*?>(.*?)</v:textbox>", inner, re.S) or [None, ""])[1]).strip()
                macro = re.search(r"<x:FmlaMacro>(.*?)</x:FmlaMacro>", inner)
                anchor = re.search(r"<x:Anchor>(.*?)</x:Anchor>", inner, re.S)
                entry = next((c for c in out if c.get("shape_id") and sa.get("id", "").endswith(c["shape_id"])), None)
                d = entry if entry is not None else {"name": sa.get("id"), "type": "Form " + ot.group(1)}
                if caption:
                    d["caption"] = caption
                if macro:
                    d["on_action"] = re.sub(r"^\[0\]!", "", macro.group(1))
                if anchor and "anchor" not in d:
                    n = [int(x) for x in re.findall(r"\d+", anchor.group(1))]
                    if len(n) >= 8:
                        d["anchor"] = {"from": {"col": n[0], "row": n[2]}, "to": {"col": n[4], "row": n[6]}}
                if entry is None:
                    out.append(d)
    return out


def extract(path: str, include_controls: bool = True, modules: Optional[list] = None, max_chars: int = 50000) -> dict:
    if not os.path.isfile(path):
        return {"error": f"File not found: {path}"}
    with open(path, "rb") as f:
        head = f.read(8)
    result: dict = {"path": path}
    sheets = []
    macros = None
    if head.startswith(b"PK"):
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            # macro project: a .bin under xl/ that starts like an OLE compound file and has the macro project structure
            for part in names:
                low = part.lower()
                if not (low.startswith("xl/") and low.endswith(".bin")):
                    continue
                data = z.read(part)
                if data[:8] != bytes.fromhex("D0CF11E0A1B11AE1"):
                    continue
                try:
                    macros = _read_macros(Cfb(data))
                except (ValueError, struct.error):
                    macros = None
                if macros is not None:
                    break
            sheets = _sheets(z)
            if include_controls:
                for s in sheets:
                    s["controls"] = _controls(z, s)
    else:
        with open(path, "rb") as f:
            cfb = Cfb(f.read())
        macros = _read_macros(cfb)
        result["note"] = ".xls: only the macro code is extracted; to get the controls, save it as .xlsm in Excel and extract again"
    if macros is None:
        result["modules"] = []
        result["message"] = "This file has no macro project"
    else:
        code_to_sheet = {s["code_name"]: s["name"] for s in sheets if s.get("code_name")}
        mods = []
        for m in macros:
            if modules and m["name"] not in modules:
                continue
            kind = m["kind"]
            if kind == "document":
                target = "main.py" if m["name"] == "ThisWorkbook" or m["name"] not in code_to_sheet and m["name"].lower().startswith("thisworkbook") else f"sheets/{m['name']}.py"
            elif kind == "userform":
                target = "(UserForm: not supported by YCGrid yet; use a custom panel, Application.Panels, instead)"
            else:
                target = f"modules/{m['name']}.py"
            code = m["code"]
            mods.append({"name": m["name"], "kind": kind, "sheet": code_to_sheet.get(m["name"]), "python_file": target,
                         "lines": code.count("\n") + 1 if code else 0, "code": code[:max_chars], "truncated": len(code) > max_chars})
        result["modules"] = mods
    if sheets:
        result["sheets"] = [{k: v for k, v in s.items() if k != "part"} for s in sheets]
    return result
