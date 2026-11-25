# SPDX-License-Identifier: Apache-2.0
r"""The call stack of a YCGrid crash that leaves no dump (fast-fail 0xC0000409, an invalid CRT parameter...).

    <YCGrid>\runtime\python\python.exe -E -s scripts\crash_stack.py <pid> [<folder with ycgrid.pdb>]

Attaches as a debugger to a running ycgrid.exe (e.g. one started with yc.launch(hidden=True)), lets every first-chance
exception through, and prints the symbolized stack of the thread that dies. From a test script:

    import crash_stack
    t, result = crash_stack.attach(p.pid)   # then reproduce; result["stack"] holds the stack text
"""
import ctypes
import os
import sys
import threading
from ctypes import wintypes as w

k = ctypes.WinDLL("kernel32", use_last_error=True)
d = ctypes.WinDLL("dbghelp")
k.OpenProcess.restype = w.HANDLE
k.OpenThread.restype = w.HANDLE
k.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]

class ADDR64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", w.WORD), ("Mode", ctypes.c_int)]

class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDR64), ("AddrReturn", ADDR64), ("AddrFrame", ADDR64), ("AddrStack", ADDR64), ("AddrBStore", ADDR64),
                ("FuncTableEntry", ctypes.c_void_p), ("Params", ctypes.c_uint64 * 4), ("Far", w.BOOL), ("Virtual", w.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 264)]

class SYM(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", w.ULONG), ("Size", w.ULONG),
                ("ModBase", ctypes.c_uint64), ("Flags", w.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", w.ULONG),
                ("Scope", w.ULONG), ("Tag", w.ULONG), ("NameLen", w.ULONG), ("MaxNameLen", w.ULONG), ("Name", ctypes.c_char * 512)]

class LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", w.DWORD), ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]

def _loop(pid, symdir, result, ready):
    k.DebugActiveProcess(pid)
    k.DebugSetProcessKillOnExit(False)
    hproc = None
    d.SymSetOptions(0x2 | 0x10 | 0x1000000)  # undecorate, load lines, defer
    ev = ctypes.create_string_buffer(1024)
    ready.set()
    while True:
        if not k.WaitForDebugEvent(ev, 1000):
            if not _alive(pid):
                return
            continue
        code = ctypes.c_uint32.from_buffer(ev, 0).value
        tid = ctypes.c_uint32.from_buffer(ev, 8).value
        cont = 0x10002  # DBG_CONTINUE
        if code == 1:  # exception
            ec = ctypes.c_uint32.from_buffer(ev, 16).value
            first = ctypes.c_uint32.from_buffer(ev, 168).value
            fatal = ec in (0xC0000409, 0xC0000005, 0xC000001D, 0xC0000094, 0xC00000FD, 0xC0000374, 0xC000041D, 0x80000003 + 0) and (first == 0 or ec in (0xC0000409, 0xC0000374, 0xC000041D))
            if ec == 0x80000003:
                fatal = False
            cont = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED
            if fatal and "done" not in result:
                result["done"] = True
                result["stack"] = _stack(pid, tid, symdir, hex(ec))
        elif code == 5:  # exit process
            return
        k.ContinueDebugEvent(pid, tid, cont)

def _alive(pid):
    h = k.OpenProcess(0x1000, False, pid)
    if not h:
        return False
    c = w.DWORD()
    k.GetExitCodeProcess(h, ctypes.byref(c))
    k.CloseHandle(h)
    return c.value == 259

def _stack(pid, tid, symdir, tag):
    hp = k.OpenProcess(0x1F0FFF, False, pid)
    ht = k.OpenThread(0x1F03FF, False, tid)
    d.SymInitialize.argtypes = [w.HANDLE, w.LPCSTR, w.BOOL]
    d.SymInitialize(hp, symdir.encode(), True)
    buf = ctypes.create_string_buffer(2048 + 16)
    addr = (ctypes.addressof(buf) + 15) & ~15
    ctx = (ctypes.c_char * 1232).from_address(addr)
    ctypes.c_uint32.from_address(addr + 0x30).value = 0x10000B
    k.GetThreadContext(ht, ctypes.c_void_p(addr))
    rip = ctypes.c_uint64.from_address(addr + 0xF8).value
    rsp = ctypes.c_uint64.from_address(addr + 0x98).value
    rbp = ctypes.c_uint64.from_address(addr + 0xA0).value
    f = STACKFRAME64()
    f.AddrPC.Offset, f.AddrPC.Mode = rip, 3
    f.AddrFrame.Offset, f.AddrFrame.Mode = rbp, 3
    f.AddrStack.Offset, f.AddrStack.Mode = rsp, 3
    d.StackWalk64.argtypes = [w.DWORD, w.HANDLE, w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    d.SymFunctionTableAccess64.restype = ctypes.c_void_p
    d.SymGetModuleBase64.restype = ctypes.c_uint64
    fta = ctypes.cast(d.SymFunctionTableAccess64, ctypes.c_void_p)
    gmb = ctypes.cast(d.SymGetModuleBase64, ctypes.c_void_p)
    lines = [f"FATAL {tag} thread {tid}"]
    for i in range(40):
        if not d.StackWalk64(0x8664, hp, ht, ctypes.byref(f), ctypes.c_void_p(addr), None, fta, gmb, None):
            break
        pc = f.AddrPC.Offset
        if not pc:
            break
        s = SYM(); s.SizeOfStruct = 88; s.MaxNameLen = 512
        disp = ctypes.c_uint64()
        d.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYM)]
        ok = d.SymFromAddr(hp, pc, ctypes.byref(disp), ctypes.byref(s))
        ln = LINE64(); ln.SizeOfStruct = ctypes.sizeof(LINE64)
        ld = w.DWORD()
        d.SymGetLineFromAddr64.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(w.DWORD), ctypes.POINTER(LINE64)]
        okl = d.SymGetLineFromAddr64(hp, pc, ctypes.byref(ld), ctypes.byref(ln))
        nm = s.Name.decode(errors="replace") if ok else hex(pc)
        lines.append(f"  #{i} {nm}" + (f"  {ln.FileName.decode(errors='replace')}:{ln.LineNumber}" if okl else ""))
    return "\n".join(lines)

def attach(pid, symdir=None):
    if symdir is None:
        symdir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist_release")
    result, ready = {}, threading.Event()
    t = threading.Thread(target=_loop, args=(pid, symdir, result, ready), daemon=True)
    t.start()
    ready.wait(5)
    return t, result


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    t, res = attach(int(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else None)
    print("attached; waiting for the process to crash or exit", flush=True)
    t.join()
    print(res.get("stack", "the process ended without a fatal exception"))
