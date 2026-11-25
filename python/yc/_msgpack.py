# SPDX-License-Identifier: Apache-2.0
"""Pure-Python MessagePack encoder/decoder (for the YCP protocol; no third-party packages).

Extension types (match the C++ side, src/io/msgpack.h):
  ext 1  Decimal: u8 scale + 16-byte signed mantissa (little-endian)  <-> decimal.Decimal
  ext 2  error value: u8 error code                                    <-> yc.XlError
  ext 3  pending (#GETTING_DATA)                                       <-> yc.Pending
"""
from __future__ import annotations

import decimal
import struct
from typing import Any

__all__ = ["packb", "unpackb", "ExtType", "Unpacker"]


class ExtType:
    __slots__ = ("code", "data")

    def __init__(self, code: int, data: bytes):
        self.code = code
        self.data = data

    def __repr__(self):
        return f"ExtType({self.code}, {self.data!r})"


# set by yc.types to avoid a circular import
ext_encoders: list = []   # [(type, fn(obj) -> (code, bytes))]
ext_decoders: dict = {}   # code -> fn(bytes) -> obj


def _pack(obj: Any, out: bytearray) -> None:
    if obj is None:
        out.append(0xC0)
    elif obj is True:
        out.append(0xC3)
    elif obj is False:
        out.append(0xC2)
    elif isinstance(obj, int):
        if 0 <= obj < 0x80:
            out.append(obj)
        elif -32 <= obj < 0:
            out.append(obj & 0xFF)
        elif 0 <= obj <= 0xFF:
            out += b"\xcc" + struct.pack(">B", obj)
        elif 0 <= obj <= 0xFFFF:
            out += b"\xcd" + struct.pack(">H", obj)
        elif 0 <= obj <= 0xFFFFFFFF:
            out += b"\xce" + struct.pack(">I", obj)
        elif 0 <= obj <= 0xFFFFFFFFFFFFFFFF:
            out += b"\xcf" + struct.pack(">Q", obj)
        elif -0x80 <= obj:
            out += b"\xd0" + struct.pack(">b", obj)
        elif -0x8000 <= obj:
            out += b"\xd1" + struct.pack(">h", obj)
        elif -0x80000000 <= obj:
            out += b"\xd2" + struct.pack(">i", obj)
        elif -0x8000000000000000 <= obj:
            out += b"\xd3" + struct.pack(">q", obj)
        else:
            # beyond 64 bits: sent as Decimal (exact)
            _pack(decimal.Decimal(obj), out)
    elif isinstance(obj, float):
        out += b"\xcb" + struct.pack(">d", obj)
    elif isinstance(obj, str):
        b = obj.encode("utf-8")
        n = len(b)
        if n < 32:
            out.append(0xA0 | n)
        elif n <= 0xFF:
            out += b"\xd9" + struct.pack(">B", n)
        elif n <= 0xFFFF:
            out += b"\xda" + struct.pack(">H", n)
        else:
            out += b"\xdb" + struct.pack(">I", n)
        out += b
    elif isinstance(obj, (bytes, bytearray, memoryview)):
        b = bytes(obj)
        n = len(b)
        if n <= 0xFF:
            out += b"\xc4" + struct.pack(">B", n)
        elif n <= 0xFFFF:
            out += b"\xc5" + struct.pack(">H", n)
        else:
            out += b"\xc6" + struct.pack(">I", n)
        out += b
    elif isinstance(obj, (list, tuple)):
        n = len(obj)
        if n < 16:
            out.append(0x90 | n)
        elif n <= 0xFFFF:
            out += b"\xdc" + struct.pack(">H", n)
        else:
            out += b"\xdd" + struct.pack(">I", n)
        for x in obj:
            _pack(x, out)
    elif isinstance(obj, dict):
        n = len(obj)
        if n < 16:
            out.append(0x80 | n)
        elif n <= 0xFFFF:
            out += b"\xde" + struct.pack(">H", n)
        else:
            out += b"\xdf" + struct.pack(">I", n)
        for k, v in obj.items():
            _pack(k, out)
            _pack(v, out)
    elif isinstance(obj, ExtType):
        _pack_ext(obj.code, obj.data, out)
    else:
        for typ, fn in ext_encoders:
            if isinstance(obj, typ):
                code, data = fn(obj)
                _pack_ext(code, data, out)
                return
        # numpy scalars, datetime etc. are converted by the caller first; strings are the last resort
        if hasattr(obj, "item"):
            _pack(obj.item(), out)
            return
        if hasattr(obj, "tolist"):
            _pack(obj.tolist(), out)
            return
        raise TypeError(f"Type can't be encoded as msgpack: {type(obj).__name__}")


def _pack_ext(code: int, data: bytes, out: bytearray) -> None:
    n = len(data)
    fixed = {1: 0xD4, 2: 0xD5, 4: 0xD6, 8: 0xD7, 16: 0xD8}
    if n in fixed:
        out.append(fixed[n])
    elif n <= 0xFF:
        out += b"\xc7" + struct.pack(">B", n)
    elif n <= 0xFFFF:
        out += b"\xc8" + struct.pack(">H", n)
    else:
        out += b"\xc9" + struct.pack(">I", n)
    out += struct.pack(">b", code)
    out += data


def packb(obj: Any) -> bytes:
    out = bytearray()
    _pack(obj, out)
    return bytes(out)


class Unpacker:
    __slots__ = ("b", "p")

    def __init__(self, data: bytes):
        self.b = memoryview(data)
        self.p = 0

    def _take(self, n: int) -> bytes:
        if self.p + n > len(self.b):
            raise ValueError("Incomplete msgpack data")
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def unpack(self) -> Any:
        t = self._take(1)[0]
        if t <= 0x7F:
            return t
        if t >= 0xE0:
            return t - 0x100
        if 0x80 <= t <= 0x8F:
            return self._map(t & 0x0F)
        if 0x90 <= t <= 0x9F:
            return self._arr(t & 0x0F)
        if 0xA0 <= t <= 0xBF:
            return bytes(self._take(t & 0x1F)).decode("utf-8")
        if t == 0xC0:
            return None
        if t == 0xC2:
            return False
        if t == 0xC3:
            return True
        if t in (0xC4, 0xC5, 0xC6):
            n = struct.unpack({0xC4: ">B", 0xC5: ">H", 0xC6: ">I"}[t], self._take({0xC4: 1, 0xC5: 2, 0xC6: 4}[t]))[0]
            return bytes(self._take(n))
        if t in (0xC7, 0xC8, 0xC9):
            n = struct.unpack({0xC7: ">B", 0xC8: ">H", 0xC9: ">I"}[t], self._take({0xC7: 1, 0xC8: 2, 0xC9: 4}[t]))[0]
            return self._ext(n)
        if t == 0xCA:
            return struct.unpack(">f", self._take(4))[0]
        if t == 0xCB:
            return struct.unpack(">d", self._take(8))[0]
        if t == 0xCC:
            return self._take(1)[0]
        if t == 0xCD:
            return struct.unpack(">H", self._take(2))[0]
        if t == 0xCE:
            return struct.unpack(">I", self._take(4))[0]
        if t == 0xCF:
            return struct.unpack(">Q", self._take(8))[0]
        if t == 0xD0:
            return struct.unpack(">b", self._take(1))[0]
        if t == 0xD1:
            return struct.unpack(">h", self._take(2))[0]
        if t == 0xD2:
            return struct.unpack(">i", self._take(4))[0]
        if t == 0xD3:
            return struct.unpack(">q", self._take(8))[0]
        if t in (0xD4, 0xD5, 0xD6, 0xD7, 0xD8):
            return self._ext({0xD4: 1, 0xD5: 2, 0xD6: 4, 0xD7: 8, 0xD8: 16}[t])
        if t in (0xD9, 0xDA, 0xDB):
            n = struct.unpack({0xD9: ">B", 0xDA: ">H", 0xDB: ">I"}[t], self._take({0xD9: 1, 0xDA: 2, 0xDB: 4}[t]))[0]
            return bytes(self._take(n)).decode("utf-8")
        if t == 0xDC:
            return self._arr(struct.unpack(">H", self._take(2))[0])
        if t == 0xDD:
            return self._arr(struct.unpack(">I", self._take(4))[0])
        if t == 0xDE:
            return self._map(struct.unpack(">H", self._take(2))[0])
        if t == 0xDF:
            return self._map(struct.unpack(">I", self._take(4))[0])
        raise ValueError(f"Unknown msgpack type 0x{t:02x}")

    def _arr(self, n: int) -> list:
        return [self.unpack() for _ in range(n)]

    def _map(self, n: int) -> dict:
        d = {}
        for _ in range(n):
            k = self.unpack()
            d[k] = self.unpack()
        return d

    def _ext(self, n: int) -> Any:
        code = struct.unpack(">b", self._take(1))[0]
        data = bytes(self._take(n))
        fn = ext_decoders.get(code)
        return fn(data) if fn else ExtType(code, data)


def unpackb(data: bytes) -> Any:
    u = Unpacker(data)
    v = u.unpack()
    return v
