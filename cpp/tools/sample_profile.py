r"""Sampling profiler for PyfaReader.exe (or any Windows x64 exe with a PDB): Windows Python, ctypes + dbghelp.

Every interval it suspends each thread of the process, walks its stack with dbghelp
(StackWalk64, symbols from the PDB next to the exe), and resumes it. Samples are tagged
with the optimizer's phase (the last "progress" line on stderr; only --optimize-request
prints them, other modes are reported as one phase). Busy samples (the top
frames aren't a wait) are reported per phase: self time, inclusive time and the most
common stacks; waiting threads count as idle, so busy / (busy + idle) is the thread use.
No build change is needed: the Release build already writes PyfaReader.pdb.

Run it with Windows Python (from WSL: python.exe), with Windows paths:
    python.exe cpp\tools\sample_profile.py <report.txt> <interval_ms> <exe> <args...>
e.g.
    python.exe cpp\tools\sample_profile.py C:\Temp\prof.txt 5 I:\koodi\pyfa\cpp\bin\Release\PyfaReader.exe \
        --optimize-request C:\Temp\req.json --out C:\Temp\resp.json

Inlined functions are attributed to their callers (Release builds inline heavily).
"""
import collections
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import threading
import time

out_path, interval = sys.argv[1], float(sys.argv[2]) / 1000
cmd = sys.argv[3:]

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbg = ctypes.WinDLL('dbghelp', use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL = 0x0002 | 0x0008 | 0x0040 | 0x0010  # suspend/resume, get context, query info
TH32CS_SNAPTHREAD = 4


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [('Low', ctypes.c_uint64), ('High', ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [('P1Home', ctypes.c_uint64), ('P2Home', ctypes.c_uint64), ('P3Home', ctypes.c_uint64),
                ('P4Home', ctypes.c_uint64), ('P5Home', ctypes.c_uint64), ('P6Home', ctypes.c_uint64),
                ('ContextFlags', wt.DWORD), ('MxCsr', wt.DWORD),
                ('SegCs', wt.WORD), ('SegDs', wt.WORD), ('SegEs', wt.WORD), ('SegFs', wt.WORD),
                ('SegGs', wt.WORD), ('SegSs', wt.WORD), ('EFlags', wt.DWORD),
                ('Dr0', ctypes.c_uint64), ('Dr1', ctypes.c_uint64), ('Dr2', ctypes.c_uint64),
                ('Dr3', ctypes.c_uint64), ('Dr6', ctypes.c_uint64), ('Dr7', ctypes.c_uint64),
                ('Rax', ctypes.c_uint64), ('Rcx', ctypes.c_uint64), ('Rdx', ctypes.c_uint64),
                ('Rbx', ctypes.c_uint64), ('Rsp', ctypes.c_uint64), ('Rbp', ctypes.c_uint64),
                ('Rsi', ctypes.c_uint64), ('Rdi', ctypes.c_uint64), ('R8', ctypes.c_uint64),
                ('R9', ctypes.c_uint64), ('R10', ctypes.c_uint64), ('R11', ctypes.c_uint64),
                ('R12', ctypes.c_uint64), ('R13', ctypes.c_uint64), ('R14', ctypes.c_uint64),
                ('R15', ctypes.c_uint64), ('Rip', ctypes.c_uint64),
                ('FltSave', ctypes.c_byte * 512),
                ('VectorRegister', M128A * 26), ('VectorControl', ctypes.c_uint64),
                ('DebugControl', ctypes.c_uint64), ('LastBranchToRip', ctypes.c_uint64),
                ('LastBranchFromRip', ctypes.c_uint64), ('LastExceptionToRip', ctypes.c_uint64),
                ('LastExceptionFromRip', ctypes.c_uint64)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [('Offset', ctypes.c_uint64), ('Segment', wt.WORD), ('Mode', ctypes.c_int)]


class KDHELP64(ctypes.Structure):
    _fields_ = [('Thread', ctypes.c_uint64), ('ThCallbackStack', wt.DWORD), ('ThCallbackBStore', wt.DWORD),
                ('NextCallback', wt.DWORD), ('FramePointer', wt.DWORD), ('KiCallUserMode', ctypes.c_uint64),
                ('KeUserCallbackDispatcher', ctypes.c_uint64), ('SystemRangeStart', ctypes.c_uint64),
                ('KiUserExceptionDispatcher', ctypes.c_uint64), ('StackBase', ctypes.c_uint64),
                ('StackLimit', ctypes.c_uint64), ('BuildVersion', wt.DWORD), ('RetpolineStubFunctionTableSize', wt.DWORD),
                ('RetpolineStubFunctionTable', ctypes.c_uint64), ('RetpolineStubOffset', wt.DWORD),
                ('RetpolineStubSize', wt.DWORD), ('Reserved0', ctypes.c_uint64 * 2)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [('AddrPC', ADDRESS64), ('AddrReturn', ADDRESS64), ('AddrFrame', ADDRESS64),
                ('AddrStack', ADDRESS64), ('AddrBStore', ADDRESS64), ('FuncTableEntry', ctypes.c_void_p),
                ('Params', ctypes.c_uint64 * 4), ('Far', wt.BOOL), ('Virtual', wt.BOOL),
                ('Reserved', ctypes.c_uint64 * 3), ('KdHelp', KDHELP64)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 1024)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
FTA = ctypes.CFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)
GMB = ctypes.CFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)
fta = FTA(lambda h, a: dbg.SymFunctionTableAccess64(h, ctypes.c_uint64(a)))
gmb = GMB(lambda h, a: dbg.SymGetModuleBase64(h, ctypes.c_uint64(a)))
dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                            ctypes.c_void_p, FTA, GMB, ctypes.c_void_p]

proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
phase = ['start']
phase_times = []
t0 = time.time()


def read_stderr():
    for raw in proc.stderr:
        line = raw.decode(errors='replace').strip()
        if line.startswith('progress '):
            p = line.split(' ', 2)[2] if len(line.split(' ', 2)) > 2 else ''
            p = p.split(':')[0].split(',')[0]
            if p != phase[0]:
                phase_times.append((time.time() - t0, p))
                phase[0] = p


threading.Thread(target=read_stderr, daemon=True).start()
hp = k32.OpenProcess(PROCESS_ALL_ACCESS, False, proc.pid)
dbg.SymSetOptions(0x2 | 0x4 | 0x10)  # undname, deferred loads, load lines off
time.sleep(0.3)
dbg.SymInitialize(hp, None, True)
names = {}


def name_of(addr):
    n = names.get(addr)
    if n is None:
        si = SYMBOL_INFO()
        si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 1024
        si.MaxNameLen = 1023
        disp = ctypes.c_uint64()
        if dbg.SymFromAddr(hp, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si)):
            n = si.Name.decode(errors='replace')
        else:
            n = '?%x' % addr
        names[addr] = n
    return n


self_count = collections.defaultdict(collections.Counter)
incl_count = collections.defaultdict(collections.Counter)
busy = collections.Counter()
idle = collections.Counter()
stacks = collections.defaultdict(collections.Counter)
WAIT = ('NtWaitFor', 'NtDelayExecution', 'NtRemoveIoCompletion', 'ZwWaitFor', 'NtReadFile',
        'NtAlertThreadByThreadId', 'NtWaitForAlertByThreadId', 'SleepConditionVariable', 'WaitOnAddress')
refreshed = 0
while proc.poll() is None:
    if time.time() - refreshed > 2:
        dbg.SymRefreshModuleList(hp)
        refreshed = time.time()
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == proc.pid:
            tids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    ph = phase[0]
    for tid in tids:
        ht = k32.OpenThread(THREAD_ALL, False, tid)
        if not ht:
            continue
        if k32.SuspendThread(ht) == 0xFFFFFFFF:
            k32.CloseHandle(ht)
            continue
        ctx = CONTEXT()
        ctx.ContextFlags = 0x10000B
        frames = []
        if k32.GetThreadContext(ht, ctypes.byref(ctx)):
            sf = STACKFRAME64()
            sf.AddrPC.Offset, sf.AddrPC.Mode = ctx.Rip, 3
            sf.AddrFrame.Offset, sf.AddrFrame.Mode = ctx.Rbp, 3
            sf.AddrStack.Offset, sf.AddrStack.Mode = ctx.Rsp, 3
            for _ in range(48):
                if not dbg.StackWalk64(0x8664, hp, ht, ctypes.byref(sf), ctypes.byref(ctx), None, fta, gmb, None):
                    break
                if not sf.AddrPC.Offset:
                    break
                frames.append(sf.AddrPC.Offset)
        k32.ResumeThread(ht)
        k32.CloseHandle(ht)
        if not frames:
            continue
        names_ = [name_of(a) for a in frames]
        if any(w in names_[0] for w in WAIT) or any(w in n for n in names_[:3] for w in WAIT):
            idle[ph] += 1
            continue
        busy[ph] += 1
        self_count[ph][names_[0]] += 1
        for n in set(names_):
            incl_count[ph][n] += 1
        stacks[ph][' <- '.join(names_[:8])] += 1
    time.sleep(interval)

with open(out_path, 'w', encoding='utf-8') as f:
    f.write('phases (s since start): %s\n' % phase_times)
    for ph in busy:
        tot = busy[ph]
        f.write('\n==== phase %s: %d busy thread samples, %d idle\n' % (ph, tot, idle[ph]))
        f.write('-- self (top 30)\n')
        for n, c in self_count[ph].most_common(30):
            f.write('%6.1f%%  %s\n' % (100 * c / tot, n))
        f.write('-- inclusive (top 45)\n')
        for n, c in incl_count[ph].most_common(45):
            f.write('%6.1f%%  %s\n' % (100 * c / tot, n))
        f.write('-- stacks (top 25)\n')
        for n, c in stacks[ph].most_common(25):
            f.write('%6.1f%%  %s\n' % (100 * c / tot, n))
print('done', proc.returncode)
