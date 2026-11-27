# SPDX-License-Identifier: Apache-2.0
r"""The call stack in a YCGrid crash or assert dump (.dmp), without a debugger installed.

    <YCGrid>\runtime\python\python.exe -E -s scripts\dump_stack.py <file.dmp> [<folder with ycgrid.pdb>]

The watchdog writes a dump for a crash, a hang and a debug build's failed CRT check (an assert, abort()), into the
Error Log's folder or %TEMP%\ycgrid-dumps (a random round's TEMP folder: copy it out before the round ends). This
reads the dump's exception (the thread and its registers), its modules and the memory it kept, and walks the stack
with dbghelp from the same folder as ycgrid.exe's .pdb (dist_debug for a debug build's dump). For a hang dump (no
exception) it walks every thread.
"""
import ctypes
import os
import struct
import sys
from ctypes import wintypes as w

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crash_stack import LINE64, STACKFRAME64, SYM  # noqa: E402

d = ctypes.WinDLL("dbghelp")
FAKE = w.HANDLE(0x5C5C5C5C)  # dbghelp keys its symbols by a process handle: any value no real process has


class Dump:
    def __init__(self, path):
        self.b = open(path, "rb").read()
        sig, _ver, n, rva = struct.unpack_from("<4sIII", self.b, 0)
        if sig != b"MDMP":
            raise SystemExit(f"{path} isn't a minidump")
        self.streams = {}
        for i in range(n):
            typ, size, at = struct.unpack_from("<III", self.b, rva + 12 * i)
            self.streams[typ] = (size, at)
        self.ranges = []  # (start, size, file offset)
        if 5 in self.streams:  # MemoryListStream
            _, at = self.streams[5]
            (count,) = struct.unpack_from("<I", self.b, at)
            for i in range(count):
                start, size, off = struct.unpack_from("<QII", self.b, at + 4 + 16 * i)
                self.ranges.append((start, size, off))
        if 9 in self.streams:  # Memory64ListStream
            _, at = self.streams[9]
            count, base = struct.unpack_from("<QQ", self.b, at)
            off = base
            for i in range(count):
                start, size = struct.unpack_from("<QQ", self.b, at + 16 + 16 * i)
                self.ranges.append((start, size, off))
                off += size
        for _, size, off in self.ranges:  # thread stacks are in the thread list too
            pass
        self.threads = []
        if 3 in self.streams:
            _, at = self.streams[3]
            (count,) = struct.unpack_from("<I", self.b, at)
            for i in range(count):
                tid, _s, _pc, _p, _teb, sstart, ssize, srva, csize, crva = struct.unpack_from("<IIIIQQIIII", self.b, at + 4 + 48 * i)
                self.ranges.append((sstart, ssize, srva))
                self.threads.append((tid, crva, csize))
        self.modules = []
        if 4 in self.streams:
            _, at = self.streams[4]
            (count,) = struct.unpack_from("<I", self.b, at)
            for i in range(count):
                base, size, _ck, _ts, name_rva = struct.unpack_from("<QIIII", self.b, at + 4 + 108 * i)
                (nlen,) = struct.unpack_from("<I", self.b, name_rva)
                name = self.b[name_rva + 4:name_rva + 4 + nlen].decode("utf-16-le")
                self.modules.append((base, size, name))
        self.exception = None
        if 6 in self.streams:
            _, at = self.streams[6]
            tid, _align, code = struct.unpack_from("<III", self.b, at)
            csize, crva = struct.unpack_from("<II", self.b, at + 8 + 152)
            self.exception = (tid, code, crva, csize)

    def read(self, addr, size):
        for start, rsize, off in self.ranges:
            if start <= addr and addr + size <= start + rsize:
                return self.b[off + addr - start:off + addr - start + size]
        return None


def walk(dump, ctx_rva, ctx_size, symdir, title):
    ctx_buf = ctypes.create_string_buffer(2048 + 16)
    addr = (ctypes.addressof(ctx_buf) + 15) & ~15
    raw = dump.b[ctx_rva:ctx_rva + ctx_size]
    ctypes.memmove(addr, raw, min(len(raw), 1232))
    rip = ctypes.c_uint64.from_address(addr + 0xF8).value
    rsp = ctypes.c_uint64.from_address(addr + 0x98).value
    rbp = ctypes.c_uint64.from_address(addr + 0xA0).value
    f = STACKFRAME64()
    f.AddrPC.Offset, f.AddrPC.Mode = rip, 3
    f.AddrFrame.Offset, f.AddrFrame.Mode = rbp, 3
    f.AddrStack.Offset, f.AddrStack.Mode = rsp, 3
    READ = ctypes.WINFUNCTYPE(w.BOOL, w.HANDLE, ctypes.c_uint64, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD))

    def read_mem(_h, base, buf, size, done):
        data = dump.read(base, size)
        if data is None:
            data = read_image(base, size)
        if data is None:
            if os.environ.get("DUMP_STACK_DEBUG"):
                print(f"  (no memory at 0x{base:X}, {size} bytes)")
            return False
        ctypes.memmove(buf, data, len(data))
        if done:
            done[0] = len(data)
        return True

    images = {}

    def read_image(addr, size):  # code bytes the dump didn't keep: from the module's file on disk
        for base, msize, name in dump.modules:
            if base <= addr < base + msize:
                if name not in images:
                    try:
                        images[name] = open(name, "rb").read()
                    except OSError:
                        images[name] = None
                img = images[name]
                if img is None:
                    return None
                rva = addr - base
                # map the RVA to a file offset through the section table
                pe = struct.unpack_from("<I", img, 0x3C)[0]
                nsec = struct.unpack_from("<H", img, pe + 6)[0]
                opt = struct.unpack_from("<H", img, pe + 20)[0]
                sec = pe + 24 + opt
                for i in range(nsec):
                    vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", img, sec + 40 * i + 8)
                    if vaddr <= rva < vaddr + max(vsize, rawsize):
                        o = rawptr + rva - vaddr
                        return img[o:o + size] if o + size <= len(img) else None
                return img[rva:rva + size] if rva + size <= len(img) else None
        return None

    reader = READ(read_mem)
    d.StackWalk64.argtypes = [w.DWORD, w.HANDLE, w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                              ctypes.c_void_p]
    # dbghelp's own function table lookup reads the module through the process handle, which a dump has none of:
    # the RUNTIME_FUNCTION of an address comes from its image's exception directory (.pdata) here
    FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, w.HANDLE, ctypes.c_uint64)
    GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, w.HANDLE, ctypes.c_uint64)
    pdata = {}
    entry = ctypes.create_string_buffer(12)

    def table_of(base, name):
        if name not in pdata:
            pdata[name] = None
            hdr = read_image(base, 0x1000)
            if hdr:
                pe = struct.unpack_from("<I", hdr, 0x3C)[0]
                opt = pe + 24
                exc_rva, exc_size = struct.unpack_from("<II", hdr, opt + 112 + 3 * 8)  # data directory 3: exceptions
                raw = read_image(base + exc_rva, exc_size) if exc_size else None
                if raw:
                    pdata[name] = [struct.unpack_from("<III", raw, i) for i in range(0, len(raw) - 11, 12)]
        return pdata[name]

    def function_entry(_h, pc):
        for base, size, name in dump.modules:
            if base <= pc < base + size:
                table = table_of(base, name)
                if not table:
                    return None
                rva = pc - base
                lo, hi = 0, len(table) - 1
                while lo <= hi:
                    mid = (lo + hi) // 2
                    b, e, u = table[mid]
                    if rva < b:
                        hi = mid - 1
                    elif rva >= e:
                        lo = mid + 1
                    else:
                        struct.pack_into("<III", entry, 0, b, e, u)
                        return ctypes.addressof(entry)
                return None
        return None

    def module_base(_h, pc):
        return next((b for b, sz, _n in dump.modules if b <= pc < b + sz), 0)

    fta_cb, gmb_cb = FTA(function_entry), GMB(module_base)
    fta = ctypes.cast(fta_cb, ctypes.c_void_p)
    gmb = ctypes.cast(gmb_cb, ctypes.c_void_p)
    d.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYM)]
    d.SymGetLineFromAddr64.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(w.DWORD), ctypes.POINTER(LINE64)]
    out = [title]
    for i in range(60):
        if not d.StackWalk64(0x8664, FAKE, None, ctypes.byref(f), ctypes.c_void_p(addr), reader, fta, gmb, None):
            break
        pc = f.AddrPC.Offset
        if not pc:
            break
        s = SYM()
        s.SizeOfStruct, s.MaxNameLen = 88, 512
        disp = ctypes.c_uint64()
        ok = d.SymFromAddr(FAKE, pc, ctypes.byref(disp), ctypes.byref(s))
        ln = LINE64()
        ln.SizeOfStruct = ctypes.sizeof(LINE64)
        ld = w.DWORD()
        okl = d.SymGetLineFromAddr64(FAKE, pc, ctypes.byref(ld), ctypes.byref(ln))
        mod = next((os.path.basename(n) for b, sz, n in dump.modules if b <= pc < b + sz), "?")
        name = s.Name.decode(errors="replace") if ok else hex(pc)
        out.append(f"  #{i} {mod}!{name}" + (f"  {ln.FileName.decode(errors='replace')}:{ln.LineNumber}" if okl else ""))
    return "\n".join(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dump = Dump(sys.argv[1])
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    symdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "dist_debug")
    d.SymSetOptions(0x2 | 0x10)  # undecorate, load lines
    d.SymInitializeW.argtypes = [w.HANDLE, w.LPCWSTR, w.BOOL]
    d.SymInitializeW(FAKE, symdir, False)
    d.SymLoadModuleExW.argtypes = [w.HANDLE, w.HANDLE, w.LPCWSTR, w.LPCWSTR, ctypes.c_uint64, w.DWORD, ctypes.c_void_p, w.DWORD]
    d.SymLoadModuleExW.restype = ctypes.c_uint64
    for base, size, name in dump.modules:
        image = name
        if os.path.basename(name).lower() == "ycgrid.exe" and os.path.exists(os.path.join(symdir, "ycgrid.exe")):
            image = os.path.join(symdir, "ycgrid.exe")
        d.SymLoadModuleExW(FAKE, None, image, None, base, size, None, 0)
    if dump.exception:
        tid, code, crva, csize = dump.exception
        print(walk(dump, crva, csize, symdir, f"exception 0x{code:08X} in thread {tid}"))
    else:
        for tid, crva, csize in dump.threads:
            print(walk(dump, crva, csize, symdir, f"thread {tid}"))
            print()


if __name__ == "__main__":
    main()
