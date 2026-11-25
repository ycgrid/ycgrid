# SPDX-License-Identifier: Apache-2.0
"""The parts of yc.mcp that need no running YCGrid: protocol, tool list, API lookup, Excel extraction."""
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc.mcp import apiref, server, tools, macro_extract  # noqa: E402


# ---- test macro project builder (an Office macro project "compressed" with literal tokens only, plus a minimal OLE compound file) ----
def macro_compress_literal(data: bytes) -> bytes:
    out = bytearray(b"\x01")
    for i in range(0, max(1, len(data)), 3584):  # each compressed chunk must stay within 4098 bytes
        chunk = data[i:i + 3584]
        body = bytearray()
        for j in range(0, len(chunk), 8):
            body.append(0)
            body += chunk[j:j + 8]
        out += struct.pack("<H", 0xB000 | (len(body) + 2 - 3)) + body
    return bytes(out)


def build_cfb(streams: dict) -> bytes:
    """streams: {"Macros/dir": bytes, ...}; one storage level. Mini stream cutoff 0: everything in regular sectors."""
    ss = 512
    sectors = []  # (data, next)
    entries = [{"name": "Root Entry", "type": 5, "child": None, "start": 0xFFFFFFFE, "size": 0, "kids": []}]
    storages = {}

    def add_entry(e):
        entries.append(e)
        return len(entries) - 1

    for path, data in streams.items():
        parent = 0
        parts = path.split("/")
        for p in parts[:-1]:
            if p not in storages:
                storages[p] = add_entry({"name": p, "type": 1, "start": 0, "size": 0, "kids": []})
                entries[parent]["kids"].append(storages[p])
            parent = storages[p]
        idx = add_entry({"name": parts[-1], "type": 2, "data": data, "kids": []})
        entries[parent]["kids"].append(idx)
    # stream data
    chains = []
    next_sec = 0
    for e in entries:
        if e.get("type") == 2:
            n = max(1, (len(e["data"]) + ss - 1) // ss)
            e["start"], e["size"] = next_sec, len(e["data"])
            chains.append((next_sec, n))
            padded = e["data"] + b"\0" * (n * ss - len(e["data"]))
            sectors += [padded[i * ss:(i + 1) * ss] for i in range(n)]
            next_sec += n
    # directory
    dir_start = next_sec
    n_dir = (len(entries) * 128 + ss - 1) // ss
    chains.append((dir_start, n_dir))
    next_sec += n_dir
    fat_sec = next_sec
    next_sec += 1
    fat = [0xFFFFFFFF] * (ss // 4)
    for start, n in chains:
        for k in range(n):
            fat[start + k] = start + k + 1 if k < n - 1 else 0xFFFFFFFE
    fat[fat_sec] = 0xFFFFFFFD
    # siblings chained through their right links
    for e in entries:
        kids = e["kids"]
        e["child"] = kids[0] if kids else 0xFFFFFFFF
        for a, b in zip(kids, kids[1:]):
            entries[a]["right"] = b
    dir_raw = bytearray()
    for e in entries:
        name = e["name"].encode("utf-16-le") + b"\0\0"
        rec = bytearray(128)
        rec[:len(name)] = name
        struct.pack_into("<H", rec, 64, len(name))
        rec[66] = e["type"]
        struct.pack_into("<III", rec, 68, 0xFFFFFFFF, e.get("right", 0xFFFFFFFF), e["child"])
        struct.pack_into("<I", rec, 116, e.get("start", 0xFFFFFFFE) if e["type"] == 2 else 0xFFFFFFFE)
        struct.pack_into("<I", rec, 120, e.get("size", 0))
        dir_raw += rec
    dir_raw += b"\0" * (n_dir * ss - len(dir_raw))
    sectors += [bytes(dir_raw[i * ss:(i + 1) * ss]) for i in range(n_dir)]
    sectors.append(struct.pack(f"<{ss // 4}I", *fat))
    hdr = bytearray(512)
    hdr[:8] = bytes.fromhex("D0CF11E0A1B11AE1")
    struct.pack_into("<HH", hdr, 0x18, 0x3E, 3)
    struct.pack_into("<HHH", hdr, 0x1C, 0xFFFE, 9, 6)
    struct.pack_into("<I", hdr, 0x2C, 1)
    struct.pack_into("<I", hdr, 0x30, dir_start)
    struct.pack_into("<I", hdr, 0x38, 0)  # cutoff 0: no mini stream
    struct.pack_into("<II", hdr, 0x3C, 0xFFFFFFFE, 0)
    struct.pack_into("<II", hdr, 0x44, 0xFFFFFFFE, 0)
    difat = [fat_sec] + [0xFFFFFFFF] * 108
    struct.pack_into("<109I", hdr, 0x4C, *difat)
    return bytes(hdr) + b"".join(sectors)


def dir_stream(modules):
    def rec(rid, data):
        return struct.pack("<HI", rid, len(data)) + data

    out = rec(0x0001, struct.pack("<I", 1)) + rec(0x0002, struct.pack("<I", 0x409)) + rec(0x0003, struct.pack("<H", 65001))
    out += struct.pack("<HI", 0x0009, 4) + b"\0" * 6
    for name, typ in modules:
        n = name.encode("utf-8")
        out += rec(0x0019, n) + rec(0x001A, n) + rec(0x0031, struct.pack("<I", 0)) + rec(0x0021 if typ == "std" else 0x0022, b"") + rec(0x002B, b"")
    return out + rec(0x0010, b"")


def build_macro_bin():
    mods = {"Module1": 'Attribute VB_Name = "Module1"\r\nSub Hello()\r\n    Range("A1").Value = "嗨"\r\nEnd Sub\r\n',
            "ThisWorkbook": 'Attribute VB_Name = "ThisWorkbook"\r\nPrivate Sub Workbook_Open()\r\nEnd Sub\r\n',
            "Sheet1": 'Attribute VB_Name = "Sheet1"\r\nPrivate Sub CommandButton1_Click()\r\n    Hello\r\nEnd Sub\r\n'}
    streams = {"Macros/dir": macro_compress_literal(dir_stream([("Module1", "std"), ("ThisWorkbook", "doc"), ("Sheet1", "doc")])),
               "PROJECT": b"Module=Module1\r\nDocument=ThisWorkbook/&H00000000\r\nDocument=Sheet1/&H00000000\r\n"}
    for name, code in mods.items():
        streams["Macros/" + name] = macro_compress_literal(code.encode("utf-8"))
    return build_cfb(streams)


class MacroExtractTest(unittest.TestCase):
    def test_decompress_spec_example(self):
        a = bytes.fromhex("0119B000616263646566676800696A6B6C6D6E6F7000717273747576" + "2E")
        self.assertEqual(macro_extract.decompress(a), b"abcdefghijklmnopqrstuv.")

    def test_roundtrip_literal(self):
        data = os.urandom(10000)
        self.assertEqual(macro_extract.decompress(macro_compress_literal(data)), data)

    def test_xlsm(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.xlsm")
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("xl/macroProject.bin", build_macro_bin())
                z.writestr("xl/workbook.xml", '<workbook xmlns:r="r"><sheets><sheet name="報價" sheetId="1" r:id="rId1"/></sheets></workbook>')
                z.writestr("xl/_rels/workbook.xml.rels", '<Relationships><Relationship Id="rId1" Target="worksheets/sheet1.xml"/></Relationships>')
                z.writestr("xl/worksheets/sheet1.xml",
                           '<worksheet><sheetPr codeName="Sheet1"/><controls><control shapeId="1025" r:id="rId2" name="CommandButton1">'
                           '<controlPr linkedCell="B2"><anchor><from><xdr:col>1</xdr:col><xdr:row>2</xdr:row></from>'
                           '<to><xdr:col>3</xdr:col><xdr:row>4</xdr:row></to></anchor></controlPr></control></controls></worksheet>')
                z.writestr("xl/worksheets/_rels/sheet1.xml.rels", '<Relationships><Relationship Id="rId2" Target="../activeX/activeX1.xml"/></Relationships>')
                z.writestr("xl/activeX/activeX1.xml", '<ax:ocx ax:classid="{D7053240-CE69-11CD-A777-00DD01143C57}"/>')
            r = macro_extract.extract(path)
        mods = {m["name"]: m for m in r["modules"]}
        self.assertIn('Range("A1").Value = "嗨"', mods["Module1"]["code"])
        self.assertNotIn("Attribute VB_Name", mods["Module1"]["code"])
        self.assertEqual(mods["Module1"]["python_file"], "modules/Module1.py")
        self.assertEqual(mods["ThisWorkbook"]["python_file"], "main.py")
        self.assertEqual(mods["Sheet1"]["python_file"], "sheets/Sheet1.py")
        self.assertEqual(mods["Sheet1"]["sheet"], "報價")
        ctl = r["sheets"][0]["controls"][0]
        self.assertEqual(ctl["type"], "ActiveX CommandButton")
        self.assertEqual(ctl["linkedCell"], "B2")
        self.assertEqual(ctl["anchor"]["from"], {"col": 1, "row": 2})


class McpProtocolTest(unittest.TestCase):
    def run_server(self, msgs):
        out = io.BytesIO()
        srv = server.Server(out)
        srv.serve(io.BytesIO(b"\n".join(json.dumps(m).encode() for m in msgs) + b"\n"))
        return [json.loads(x) for x in out.getvalue().splitlines()]

    def test_initialize_and_list(self):
        res = self.run_server([
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "yc_api_lookup", "arguments": {"query": "Range.End"}}},
            {"jsonrpc": "2.0", "id": 4, "method": "nope"},
        ])
        self.assertEqual(len(res), 4)  # a notification gets no response
        self.assertEqual(res[0]["result"]["protocolVersion"], "2025-06-18")
        names = {t["name"] for t in res[1]["result"]["tools"]}
        self.assertEqual(len(names), 42)
        self.assertEqual(names, set(tools.HANDLERS))  # every tool has a handler
        body = json.loads(res[2]["result"]["content"][0]["text"])
        self.assertTrue(body["object_model"]["implemented"])
        self.assertEqual(res[3]["error"]["code"], -32601)

    def test_api_lookup_missing(self):
        r = apiref.lookup("Range.NoSuchMember")
        self.assertFalse(r["object_model"]["implemented"])
        r = apiref.lookup("xlUp")
        self.assertEqual(r["object_model"]["kind"], "constant")

    def test_key_tokens(self):
        self.assertEqual(tools._key_token("Ctrl+Shift+L"), "k:c-s-L")
        self.assertEqual(tools._key_token("Alt+Down"), "k:a-DownArrow")
        self.assertEqual(tools._key_token("F9"), "k:F9")


class ControlEventTest(unittest.TestCase):
    def test_handler_names_follow_the_event_ycgrid_fires(self):
        """The handler the tools name and check (yc_list_controls, yc_add_control, yc_python_status) is the one YCGrid
        runs: a check box's is _Click, as Excel's View Code opens; it was checked as _Change and reported missing."""
        import re
        from yc.ui import _SIZES
        src = os.path.join(os.path.dirname(__file__), "..", "..", "apps", "ycgrid", "objects_ui.cpp")
        with open(src, encoding="utf-8") as f:
            body = f.read().split("DefaultControlEvent(ControlKind k)", 1)[1].split("default:", 1)[0]
        change = {m.lower() for m in re.findall(r"ControlKind::(\w+):", body)}
        self.assertTrue(change, "DefaultControlEvent not found")
        for kind in _SIZES:
            self.assertEqual(tools._default_event(kind), "Change" if kind in change else "Click", kind)
        self.assertEqual(tools._default_event("checkbox"), "Click")


if __name__ == "__main__":
    unittest.main()
