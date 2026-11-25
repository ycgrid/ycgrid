# SPDX-License-Identifier: Apache-2.0
r"""Where a running YCGrid spends its time while the screen stutters: one thread's stack, sampled every few ms.

    <YCGrid>\runtime\python\python.exe -E -s scripts\ui_sampler.py <pid> [--seconds 6] [--thread yc-engine]
        [--symbols <folder with ycgrid.pdb>]

Suspends the thread (the UI thread by default, the process's first; --thread takes the name YCGrid gives one of its
threads: yc-engine, yc-engine-release, yc-snapshot-release, yc-python-host, yc-controller, yc-feed), walks its stack
with dbghelp and ycgrid.pdb (from the exe's folder unless --symbols says otherwise), and prints the functions by the
share of samples they are on (inclusive), the ones on top (exclusive) and the most common stacks. A frame that waits
for the engine shows std::future<...>::get under Engine::Call; an idle UI thread waits in
NtUserMsgWaitForMultipleObjectsEx. Sample the engine thread next to see what it was busy with.

From a script, around the slow step (see skill/ycgrid-python/references/testing.md):

    import ui_sampler
    run = ui_sampler.start(proc.pid, 6.0)            # or thread="yc-engine"
    ...the step...
    ui_sampler.report(run.result())

Only for YCGrid processes of your own (a hidden instance): it stops the thread hundreds of times a second.
"""
from __future__ import annotations

import argparse
import collections
import ctypes
import os
import sys
import threading
import time
from ctypes import wintypes as w

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crash_stack import STACKFRAME64, SYM  # noqa: E402  (the same dbghelp structures)

k = ctypes.WinDLL("kernel32", use_last_error=True)
d = ctypes.WinDLL("dbghelp")
k.OpenProcess.restype = w.HANDLE
k.OpenThread.restype = w.HANDLE
k.CreateToolhelp32Snapshot.restype = w.HANDLE
k.GetThreadDescription.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
d.SymInitialize.argtypes = [w.HANDLE, w.LPCSTR, w.BOOL]
d.StackWalk64.argtypes = [w.DWORD, w.HANDLE, w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                          ctypes.c_void_p, ctypes.c_void_p]
d.SymFunctionTableAccess64.restype = ctypes.c_void_p
d.SymGetModuleBase64.restype = ctypes.c_uint64
d.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYM)]

IDLE = ("Wait", "Sleep", "NtUser", "ZwWait", "NtWait", "GetMessage", "Present", "DispatchMessage")


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD), ("th32OwnerProcessID", w.DWORD),
                ("tpBasePri", w.LONG), ("tpDeltaPri", w.LONG), ("dwFlags", w.DWORD)]


def threads_of(pid):
    """The process's thread ids, the first one (the UI thread) first."""
    snap = k.CreateToolhelp32Snapshot(0x4, 0)  # TH32CS_SNAPTHREAD
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k.Thread32Next(snap, ctypes.byref(te))
    k.CloseHandle(snap)
    return out


def thread_named(pid, name):
    """The thread YCGrid named name (SetThreadDescription), or None."""
    for tid in threads_of(pid):
        ht = k.OpenThread(0x0800, False, tid)  # THREAD_QUERY_LIMITED_INFORMATION
        if not ht:
            continue
        p = ctypes.c_wchar_p()
        try:
            if k.GetThreadDescription(ht, ctypes.byref(p)) >= 0 and p.value == name:
                return tid
        finally:
            k.CloseHandle(ht)
    return None


def exe_folder(pid):
    hp = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not hp:
        return None
    try:
        buf = ctypes.create_unicode_buffer(32768)
        n = w.DWORD(len(buf))
        return os.path.dirname(buf.value) if k.QueryFullProcessImageNameW(hp, 0, buf, ctypes.byref(n)) else None
    finally:
        k.CloseHandle(hp)


class Result:
    def __init__(self):
        self.inclusive, self.exclusive, self.stacks = collections.Counter(), collections.Counter(), collections.Counter()
        self.samples = 0


def sample(pid, seconds, thread=None, interval=0.005, symbols=None, depth=40):
    """Samples the thread (a name, a thread id, or None for the UI thread) for seconds; returns a Result."""
    res = Result()
    tid = thread_named(pid, thread) if isinstance(thread, str) else thread
    if tid is None:
        if thread is not None:
            raise ValueError(f"no thread named {thread!r} in process {pid}")
        tid = threads_of(pid)[0]
    symbols = symbols or exe_folder(pid) or "."
    hp = k.OpenProcess(0x1F0FFF, False, pid)
    ht = k.OpenThread(0x1F03FF, False, tid)
    if not hp or not ht:
        raise OSError(f"can't open process {pid} / thread {tid}")
    d.SymSetOptions(0x2 | 0x4)  # undecorated names, deferred loads
    d.SymInitialize(hp, symbols.encode(), True)
    fta = ctypes.cast(d.SymFunctionTableAccess64, ctypes.c_void_p)
    gmb = ctypes.cast(d.SymGetModuleBase64, ctypes.c_void_p)
    names = {}

    def name(pc):
        if pc not in names:
            s = SYM()
            s.SizeOfStruct = 88
            s.MaxNameLen = 512
            disp = ctypes.c_uint64()
            names[pc] = s.Name.decode(errors="replace") if d.SymFromAddr(hp, pc, ctypes.byref(disp), ctypes.byref(s)) else hex(pc)
        return names[pc]

    buf = ctypes.create_string_buffer(2048 + 16)
    ctx = (ctypes.addressof(buf) + 15) & ~15  # CONTEXT is 16-byte aligned
    t_end = time.time() + seconds
    try:
        while time.time() < t_end:
            if k.SuspendThread(ht) == 0xFFFFFFFF:
                break  # the thread ended
            pcs = []
            try:
                ctypes.memset(ctx, 0, 1232)
                ctypes.c_uint32.from_address(ctx + 0x30).value = 0x10000B  # CONTEXT_FULL (AMD64)
                if k.GetThreadContext(ht, ctypes.c_void_p(ctx)):
                    f = STACKFRAME64()
                    f.AddrPC.Offset, f.AddrPC.Mode = ctypes.c_uint64.from_address(ctx + 0xF8).value, 3     # Rip
                    f.AddrFrame.Offset, f.AddrFrame.Mode = ctypes.c_uint64.from_address(ctx + 0xA0).value, 3  # Rbp
                    f.AddrStack.Offset, f.AddrStack.Mode = ctypes.c_uint64.from_address(ctx + 0x98).value, 3  # Rsp
                    for _ in range(depth):
                        if not d.StackWalk64(0x8664, hp, ht, ctypes.byref(f), ctypes.c_void_p(ctx), None, fta, gmb, None):
                            break
                        if not f.AddrPC.Offset:
                            break
                        pcs.append(f.AddrPC.Offset)
            finally:
                k.ResumeThread(ht)
            frames = [name(pc) for pc in pcs]
            if frames:
                res.samples += 1
                res.exclusive[frames[0]] += 1
                for nm in set(frames):
                    res.inclusive[nm] += 1
                res.stacks[" < ".join(frames[:8])] += 1
            time.sleep(interval)
    finally:
        k.CloseHandle(ht)
        d.SymCleanup(hp)
        k.CloseHandle(hp)
    return res


class _Run:
    def __init__(self, target):
        self._out = {}
        self._t = threading.Thread(target=lambda: self._out.setdefault("r", target()), daemon=True)
        self._t.start()

    def result(self):
        self._t.join()
        return self._out.get("r") or Result()


def start(pid, seconds, thread=None, **kw):
    """sample() on a thread of its own, so a script can run the slow step meanwhile; .result() waits for it."""
    return _Run(lambda: sample(pid, seconds, thread, **kw))


def report(res, top=30, stacks=8, out=sys.stdout):
    n = max(res.samples, 1)
    busy = sum(c for f, c in res.exclusive.items() if not f.startswith(IDLE))
    print(f"{res.samples} samples, {busy * 100 / n:.0f}% of them busy", file=out)
    print("inclusive:", file=out)
    for nm, c in res.inclusive.most_common(top):
        print(f"  {c * 100 / n:5.1f}%  {nm[:150]}", file=out)
    print("exclusive:", file=out)
    for nm, c in res.exclusive.most_common(15):
        print(f"  {c * 100 / n:5.1f}%  {nm[:150]}", file=out)
    print("stacks:", file=out)
    for st, c in res.stacks.most_common(stacks):
        print(f"  {c:5d}  {st[:400]}", file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pid", type=int)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--thread", default=None, help="a thread's name (yc-engine...); the UI thread when left out")
    ap.add_argument("--symbols", default=None, help="the folder with ycgrid.pdb (default: the exe's)")
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args(argv)
    report(sample(a.pid, a.seconds, a.thread, symbols=a.symbols), top=a.top)


if __name__ == "__main__":
    main()
