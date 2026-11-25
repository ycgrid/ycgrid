# SPDX-License-Identifier: Apache-2.0
"""yc.data: column frames, formats and table-like inputs (no YCGrid needed)."""
import datetime as dt
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from yc import data  # noqa: E402


def decode(frame: bytes):
    """A Python reader of the frame format (the C++ decoder is engine/bind.cpp)."""
    magic, ver, op, ncols, nrows, key, name_off, name_len, _, sent, total = data._HDR.unpack_from(frame, 0)
    assert magic == data._MAGIC and ver == 1 and total == len(frame) and total % 8 == 0
    table = frame[name_off:name_off + name_len].decode()
    cols = {}
    for i in range(ncols):
        n_off, n_len, t, v_off, v_len, o_off, valid = data._DESC.unpack_from(frame, data._HDR.size + i * data._DESC.size)
        name = frame[n_off:n_off + n_len].decode()
        assert v_off % 8 == 0 and (o_off % 8 == 0) and (valid % 8 == 0)
        ok = [True] * nrows if not valid else [(frame[valid + j // 8] >> (j % 8)) & 1 == 1 for j in range(nrows)]
        if t == data._F64:
            vals = list(struct.unpack_from(f"<{nrows}d", frame, v_off))
        elif t == data._I64:
            vals = list(struct.unpack_from(f"<{nrows}q", frame, v_off))
        elif t == data._BOOL:
            vals = [bool((frame[v_off + j // 8] >> (j % 8)) & 1) for j in range(nrows)]
        elif t == data._UTF8:
            offs = struct.unpack_from(f"<{nrows + 1}i", frame, o_off)
            vals = [frame[v_off + offs[j]:v_off + offs[j + 1]].decode() for j in range(nrows)]
        else:
            vals = [None] * nrows
        cols[name] = (t, [v if k else None for v, k in zip(vals, ok)])
    return table, op, key, sent, cols


class FrameTest(unittest.TestCase):
    def test_lists(self):
        frame, dates = data.encode_frame("Chain", data.UPSERT, ["k", "bid", "ok", "note", "when", "empty"],
                                         [[23000, 23100], [1.5, None], [True, False], ["a", "中"],
                                          [dt.datetime(2026, 1, 2, 12), None], [None, None]], key_col=0, sent_ns=42)
        table, op, key, sent, cols = decode(frame)
        self.assertEqual((table, op, key, sent), ("Chain", data.UPSERT, 0, 42))
        self.assertEqual(cols["k"][1], [23000, 23100])
        self.assertEqual(cols["bid"][1][0], 1.5)
        self.assertIsNone(cols["bid"][1][1])
        self.assertEqual(cols["ok"], (data._BOOL, [True, False]))
        self.assertEqual(cols["note"], (data._UTF8, ["a", "中"]))
        self.assertAlmostEqual(cols["when"][1][0], 46024.5)
        self.assertEqual(cols["empty"][0], data._NULL)
        self.assertEqual(dates, [False, False, False, False, True, False])

    def test_numpy_and_pandas(self):
        try:
            import numpy as np
            import pandas as pd
        except ImportError:
            self.skipTest("numpy / pandas not installed")
        df = pd.DataFrame({"strike": np.array([1, 2, 3], dtype=np.int32), "iv": [0.2, np.nan, 0.3], "flag": [True, False, True],
                           "sym": ["A", None, "C"], "t": pd.to_datetime(["2026-01-02", "2026-01-03", None])})
        names, cols = data._columns(df)
        self.assertEqual(names, ["strike", "iv", "flag", "sym", "t"])
        frame, dates = data.encode_frame("T", data.REPLACE, names, cols)
        _, _, key, _, out = decode(frame)
        self.assertEqual(key, 0xFFFFFFFF)
        self.assertEqual(out["strike"], (data._I64, [1, 2, 3]))
        self.assertTrue(out["iv"][1][1] != out["iv"][1][1])  # NaN = missing
        self.assertEqual(out["flag"][1], [True, False, True])
        self.assertEqual(out["sym"][1], ["A", None, "C"])
        self.assertAlmostEqual(out["t"][1][0], 46024.0)
        self.assertEqual(dates[4], True)
        # a dict of numpy columns, a list of dicts
        self.assertEqual(data._columns({"a": np.arange(3)})[0], ["a"])
        self.assertEqual(data._columns([{"a": 1}, {"a": 2, "b": 3}]), (["a", "b"], [[1, 2], [None, 3]]))

    def test_lengths_must_match(self):
        with self.assertRaises(data.YcError):
            data.encode_frame("T", data.UPSERT, ["a", "b"], [[1, 2], [1]])


class FormatTest(unittest.TestCase):
    def test_decode_frame(self):
        frame, _ = data.encode_frame("T", data.REPLACE, ["n", "s", "b", "d"],
                                     [[1.5, None, 3.0], ["x", None, "中"], [True, False, None], [46024.5, 46025.0, None]])
        table, cols = data.decode_frame(frame, [False, False, False, True])
        self.assertEqual(table, "T")
        self.assertEqual(cols["s"], ["x", None, "中"])
        try:
            import numpy as np
        except ImportError:
            self.assertEqual(cols["n"], [1.5, None, 3.0])
            return
        self.assertEqual(cols["n"][0], 1.5)
        self.assertTrue(np.isnan(cols["n"][1]))
        self.assertEqual(list(cols["b"]), [True, False, None])
        self.assertEqual(cols["d"][0], np.datetime64("2026-01-02T12:00:00.000"))
        self.assertTrue(np.isnat(cols["d"][2]))

    def test_specs(self):
        self.assertEqual(data.number_format("pct"), "0.00%")
        self.assertEqual(data.number_format("pct1"), "0.0%")
        self.assertEqual(data.number_format("bp"), '[mul=10000]0.0" bp"')
        self.assertEqual(data.number_format("bp2"), '[mul=10000]0.00" bp"')
        self.assertEqual(data.number_format({"tick": 0.05}), "#,##0.00")
        self.assertEqual(data.number_format({"tick": 1}), "#,##0")
        self.assertEqual(data.number_format({"tick": 0.00001}), "#,##0.00000")  # repr is "1e-05"
        self.assertEqual(data.number_format({"max_hz": 4}), "")
        self.assertEqual(data.number_format({"format": "pct1", "max_hz": 2}), "0.0%")
        self.assertEqual(data._column_defs(["bid"], {"bid": {"max_hz": 4}}, None, ()), [{"name": "bid", "number_format": "", "max_hz": 4}])
        with self.assertRaises(Exception):
            data.number_format({"tik": 0.05})
        self.assertEqual(data.number_format({"decimals": 3, "thousands": False}), "0.000")
        self.assertEqual(data.number_format("price4"), "#,##0.0000")
        self.assertEqual(data.number_format("0.000"), "0.000")
        defs = data._column_defs(["bid", "ask", "t"], {"bid": {"tick": 0.5}}, ["bid"], {"t"})
        self.assertEqual(defs[0]["number_format"], "#,##0.0")
        self.assertEqual(defs[0]["flash"], 1)
        self.assertEqual(defs[1]["number_format"], "yyyy-mm-dd hh:mm:ss")
        self.assertEqual(data._color("#2e7d32"), 0xFF2E7D32)
        self.assertEqual(data._color(255), 0xFFFF0000)


class SyncTest(unittest.TestCase):
    """BoundTable.sync: only the difference against the previous snapshot is sent."""

    def make(self):
        sent = []

        class T(data.BoundTable):
            def _send(self, op, d, wait):
                sent.append((op, {k: list(v.tolist() if hasattr(v, "tolist") else v) for k, v in d.items()}))

        return T("Pos", key="sym"), sent

    def test_diff(self):
        t, sent = self.make()
        r = t.sync({"sym": ["A", "B", "C"], "qty": [1, 2, 3], "px": [10.0, float("nan"), 30.0]})
        self.assertEqual((sent[0][0], r["added"]), (data.REPLACE, 3))
        sent.clear()
        # nothing changed (NaN equals NaN): nothing is sent
        r = t.sync({"sym": ["A", "B", "C"], "qty": [1, 2, 3], "px": [10.0, float("nan"), 30.0]})
        self.assertEqual(sent, [])
        self.assertEqual(r, {"added": 0, "changed": 0, "removed": 0, "cells": 0})
        # B's qty changes, C is gone, D is new; rows may come in any order
        r = t.sync({"sym": ["D", "B", "A"], "qty": [7, 5, 1], "px": [1.0, float("nan"), 10.0]})
        self.assertEqual(r, {"added": 1, "changed": 1, "removed": 1, "cells": 2 + 3 + 1})
        self.assertEqual(sent[0], (data.UPSERT, {"sym": ["B"], "qty": [5]}))
        self.assertEqual(sent[1], (data.UPSERT, {"sym": ["D"], "qty": [7], "px": [1.0]}))
        self.assertEqual(sent[2], (data.DELETE, {"sym": ["C"]}))
        sent.clear()
        # keep rows that are missing from a partial snapshot
        r = t.sync({"sym": ["A"], "qty": [2], "px": [10.0]}, delete_missing=False)
        self.assertEqual(r["removed"], 0)
        self.assertEqual(sent, [(data.UPSERT, {"sym": ["A"], "qty": [2]})])

    def test_needs_key(self):
        t, _ = self.make()
        with self.assertRaises(Exception):
            t.sync({"qty": [1]})


class ColumnFilesTest(unittest.TestCase):
    """save_columns: the .npy files a Data View and plot_files read, from pandas, pyarrow-like tables and dicts."""

    def test_pandas_and_arrow_tables(self):
        import numpy as np
        import pandas as pd

        df = pd.DataFrame({"t": pd.date_range("2026-01-01", periods=3, freq="min"), "px": [1.5, 2.5, 3.5],
                           "side": ["B", "S", "B"], "n": [1, 2, 3]})
        files = data.save_columns(df)
        self.assertEqual(list(files), ["t", "px", "side", "n"])
        self.assertEqual(np.load(files["t"]).dtype, np.dtype("datetime64[ns]"))
        self.assertEqual(list(np.load(files["side"])), ["B", "S", "B"])

        class Column:  # what pyarrow's ChunkedArray offers here
            def __init__(self, values):
                self.values = values

            def to_numpy(self):
                return np.asarray(self.values)

        class ArrowTable:  # duck-typed pyarrow.Table (pyarrow isn't in YCGrid's runtime)
            column_names = ["px", "side"]

            def column(self, name):
                return Column(df[name].tolist())

        files = data.save_columns(ArrowTable())
        self.assertEqual(list(files), ["px", "side"])
        self.assertEqual(list(np.load(files["px"])), [1.5, 2.5, 3.5])
        self.assertEqual(list(np.load(files["side"])), ["B", "S", "B"])

    def test_small_dtypes_and_bytes_stay_as_they_are(self):
        # bytes columns (dtype S) were turned into float64 and failed; YCGrid reads them, and 1- and 2-byte numbers, as they are
        import numpy as np

        cols = {"s": np.array([b"ab", b"c"]), "i2": np.array([1, 2], dtype=np.int16), "f2": np.array([0.5, 1], dtype=np.float16)}
        files = data.save_columns(cols)
        self.assertEqual(np.load(files["s"]).dtype.kind, "S")
        self.assertEqual(np.load(files["i2"]).dtype, np.dtype("int16"))
        self.assertEqual(np.load(files["f2"]).dtype, np.dtype("float16"))


if __name__ == "__main__":
    unittest.main()
