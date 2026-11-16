"""crashdump.py - a GT3 PC crash read off the game (PC, 2026-11-07).

    python -I -X utf8 tools\\crashdump.py [dump] [gt3pc+offset ...]

The dump: the one given, else the newest gt3pc dump of
%LOCALAPPDATA%\\CrashDumps (Windows writes one for every crash; the game's own
build\\bin\\gt3pc_crash.dmp is missing after an out-of-memory crash).  From
it: the process's memory counters at the crash (virtual = the 32-bit address
space in use, of 4096 MB), the exception record and, for a C++ exception
(0xE06D7363), the thrown type's name (std::bad_alloc: new failed) read from
the exe's throw info, and the thread names.  The frames: the offsets given,
else those of the last `crash:` block of build\\bin\\gt3pc.prev.log (the run
that crashed; gt3pc.log when it has one), symbolized with dbghelp from
build\\bin\\gt3pc.pdb - the exe and the PDB must be the build that crashed.
(The game's own report prints bare offsets when dbghelp could not load the
PDB in the crashing process, as it cannot once the memory is out.)"""
import ctypes, ctypes.wintypes as wt, glob, os, re, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
bin_dir = os.path.join(root, "build", "bin")
exe_path = os.path.join(bin_dir, "gt3pc.exe")

args = sys.argv[1:]
dump_path = None
offsets = []
for a in args:
    if a.lower().endswith(".dmp"):
        dump_path = a
    else:
        offsets.append(int(a.replace("gt3pc+", "").replace("0x", ""), 16))
if dump_path is None:
    own = os.path.join(bin_dir, "gt3pc_crash.dmp")
    cands = glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""), "CrashDumps", "gt3pc*.dmp"))
    if os.path.isfile(own) and os.path.getsize(own) > 0:
        cands.append(own)
    if not cands:
        sys.exit("no dump: give one (%LOCALAPPDATA%\\CrashDumps\\gt3pc.exe.<pid>.dmp)")
    dump_path = max(cands, key=os.path.getmtime)
print("dump: %s" % dump_path)
if not offsets:
    for log in ("gt3pc.log", "gt3pc.prev.log"):
        p = os.path.join(bin_dir, log)
        if not os.path.isfile(p):
            continue
        text = open(p, "r", encoding="utf-8", errors="replace").read()
        blocks = re.findall(r"crash: exception[^\n]*\n(?:crash:[^\n]*\n)*", text)
        if blocks:
            offsets = [int(m, 16) for m in re.findall(r"\[esp\+0x[0-9A-Fa-f]+\] gt3pc\+0x([0-9A-Fa-f]+)", blocks[-1])]
            print("frames from %s's last crash block (%d)" % (log, len(offsets)))
            break

# ---- the minidump ---------------------------------------------------------
data = open(dump_path, "rb").read()
sig, ver, nstreams, dir_rva, cks, ts, flags = struct.unpack_from("<4sIIIIIQ", data, 0)
if sig != b"MDMP":
    sys.exit("not a minidump")
streams = {}
for i in range(nstreams):
    stype, size, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
    streams.setdefault(stype, []).append((size, rva))
MB = 1048576.0

modules = []
if 4 in streams:
    size, rva = streams[4][0]
    (n,) = struct.unpack_from("<I", data, rva)
    for i in range(n):
        off = rva + 4 + 108 * i
        base, isize, cksum, mts, name_rva = struct.unpack_from("<QIIII", data, off)
        (nlen,) = struct.unpack_from("<I", data, name_rva)
        name = data[name_rva + 4:name_rva + 4 + nlen].decode("utf-16-le", "replace")
        modules.append((base, isize, name))
exe_base = None
for base, isize, name in modules:
    if name.lower().endswith("gt3pc.exe"):
        exe_base = base
        print("gt3pc.exe at 0x%X, size 0x%X" % (base, isize))

if 22 in streams:
    size, rva = streams[22][0]
    rev, fl = struct.unpack_from("<HH", data, rva)
    (pf,) = struct.unpack_from("<I", data, rva + 4)
    fields = ["PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
              "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage"]
    if rev >= 2:
        fields += ["PeakVirtualSize", "VirtualSize", "PrivateUsage"]
    vals = dict(zip(fields, struct.unpack_from("<%dQ" % len(fields), data, rva + 8)))
    print("memory at the crash: virtual %.0f MB (peak %.0f; the address space, 4096 for a large-address-aware"
          " 32-bit program), private %.0f MB, working set %.0f MB" %
          (vals.get("VirtualSize", 0) / MB, vals.get("PeakVirtualSize", 0) / MB, vals.get("PrivateUsage", 0) / MB,
           vals.get("WorkingSetSize", 0) / MB))

if 24 in streams:
    size, rva = streams[24][0]
    (n,) = struct.unpack_from("<I", data, rva)
    names = {}
    for i in range(n):
        tid, name_rva = struct.unpack_from("<IQ", data, rva + 4 + 12 * i)
        (nlen,) = struct.unpack_from("<I", data, name_rva)
        names[tid] = data[name_rva + 4:name_rva + 4 + nlen].decode("utf-16-le", "replace")
else:
    names = {}

ranges = []
if 5 in streams:
    size, rva = streams[5][0]
    (n,) = struct.unpack_from("<I", data, rva)
    for i in range(n):
        start, dsize, drva = struct.unpack_from("<QII", data, rva + 4 + 16 * i)
        ranges.append((start, dsize, drva))
if 9 in streams:
    size, rva = streams[9][0]
    n, base_rva = struct.unpack_from("<QQ", data, rva)
    pos = base_rva
    for i in range(n):
        start, dsize = struct.unpack_from("<QQ", data, rva + 16 + 16 * i)
        ranges.append((start, dsize, pos))
        pos += dsize

def dump_read(va, n):
    for start, dsize, drva in ranges:
        if start <= va and va + n <= start + dsize:
            o = drva + (va - start)
            return data[o:o + n]
    return None

if 6 in streams:
    size, rva = streams[6][0]
    tid, _al = struct.unpack_from("<II", data, rva)
    code, eflags, erec, eaddr, nparams, _u = struct.unpack_from("<IIQQII", data, rva + 8)
    params = struct.unpack_from("<15Q", data, rva + 8 + 32)
    print("exception 0x%08X at 0x%X on thread %d%s" % (code, eaddr, tid, " (%s)" % names[tid] if tid in names else ""))
    if code == 0xC0000005 and nparams >= 2:
        print("  access violation: %s address 0x%X" %
              ({0: "reading", 1: "writing", 8: "executing"}.get(params[0], "?"), params[1]))
    if code == 0xE06D7363 and nparams >= 3 and os.path.isfile(exe_path):
        obj, throw_info = params[1], params[2]
        exe = open(exe_path, "rb").read()
        pe = struct.unpack_from("<I", exe, 0x3C)[0]
        nsec = struct.unpack_from("<H", exe, pe + 6)[0]
        opt = pe + 24
        image_base = struct.unpack_from("<I", exe, opt + 28)[0]
        opt_size = struct.unpack_from("<H", exe, pe + 20)[0]
        secs = []
        for i in range(nsec):
            s = opt + opt_size + 40 * i
            vsize, va, rsize, raw = struct.unpack_from("<IIII", exe, s + 8)
            secs.append((va, vsize, raw, rsize))

        def file_read(va_pref, n):
            r = va_pref - image_base
            for va, vsize, raw, rsize in secs:
                if va <= r < va + vsize:
                    o = raw + (r - va)
                    b = exe[o:o + n]
                    return b if len(b) == n else None
            return None

        def read_chain(read, start):
            # ThrowInfo: attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray
            # (x86: absolute pointers, in `start''s address space)
            ti = read(start, 16)
            if ti is None:
                return None
            attrs, unwind, fwd, cta = struct.unpack("<IIII", ti)
            arr = read(cta, 8)
            if arr is None:
                return None
            n, first = struct.unpack("<II", arr)
            ct = read(first, 8)
            if ct is None:
                return None
            props, ptype = struct.unpack("<II", ct)
            nm = read(ptype + 8, 128)
            if nm is None:
                return None
            return nm.split(b"\0", 1)[0].decode("ascii", "replace")

        chain = read_chain(dump_read, throw_info)
        if chain is None and exe_base is not None:
            chain = read_chain(file_read, throw_info - exe_base + image_base)
        print("  a C++ exception nobody caught: %s" %
              ("thrown type %s" % chain if chain else "the thrown type is not readable (ThrowInfo 0x%X)" % throw_info))
        o = dump_read(obj, 12)
        if o is not None:
            vptr, what, _ = struct.unpack("<IIi", o)
            w = dump_read(what, 128) if what else None
            if w:
                print("  what(): %s" % w.split(b"\0", 1)[0].decode("ascii", "replace"))

# ---- the frames -----------------------------------------------------------
if not offsets:
    print("no frames to symbolize (no crash block in the logs; give gt3pc+offset values)")
    sys.exit(0)
if not os.path.isfile(exe_path):
    sys.exit("no %s" % exe_path)
dbghelp = ctypes.WinDLL("dbghelp.dll", use_last_error=True)
SYMOPT_UNDNAME, SYMOPT_LOAD_LINES = 0x2, 0x10
dbghelp.SymSetOptions.restype = wt.DWORD
dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES)
h = ctypes.c_void_p(0x4711)
dbghelp.SymInitializeW.argtypes = [ctypes.c_void_p, wt.LPCWSTR, wt.BOOL]
dbghelp.SymInitializeW.restype = wt.BOOL
if not dbghelp.SymInitializeW(h, bin_dir, False):
    sys.exit("SymInitialize failed (%d)" % ctypes.get_last_error())
dbghelp.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64,
                                     wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
base = dbghelp.SymLoadModuleExW(h, None, exe_path, None, exe_base if exe_base else 0x400000, 0, None, 0)
if base == 0:
    sys.exit("SymLoadModuleEx failed (%d)" % ctypes.get_last_error())

class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("TypeIndex", wt.DWORD), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.DWORD), ("Size", wt.DWORD), ("ModBase", ctypes.c_uint64), ("Flags", wt.DWORD),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.DWORD),
                ("Scope", wt.DWORD), ("Tag", wt.DWORD), ("NameLen", wt.DWORD), ("MaxNameLen", wt.DWORD),
                ("Name", ctypes.c_wchar * 512)]

class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", wt.LPWSTR), ("Address", ctypes.c_uint64)]

dbghelp.SymFromAddrW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                 ctypes.POINTER(SYMBOL_INFOW)]
dbghelp.SymFromAddrW.restype = wt.BOOL
dbghelp.SymGetLineFromAddrW64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                          ctypes.POINTER(IMAGEHLP_LINEW64)]
dbghelp.SymGetLineFromAddrW64.restype = wt.BOOL
print("frames (gt3pc+offset: function + displacement, file:line), nearest the crash first:")
for off in offsets:
    sym = SYMBOL_INFOW()
    sym.SizeOfStruct = 88
    sym.MaxNameLen = 512
    disp = ctypes.c_uint64(0)
    addr = base + off
    if dbghelp.SymFromAddrW(h, addr, ctypes.byref(disp), ctypes.byref(sym)):
        name = "%s + 0x%X" % (sym.Name, disp.value)
    else:
        name = "? (error %d)" % ctypes.get_last_error()
    line = IMAGEHLP_LINEW64()
    line.SizeOfStruct = ctypes.sizeof(line)
    ldisp = wt.DWORD(0)
    where = ""
    if dbghelp.SymGetLineFromAddrW64(h, addr, ctypes.byref(ldisp), ctypes.byref(line)):
        f = line.FileName
        if f.lower().startswith(root.lower()):
            f = f[len(root) + 1:]
        where = "  %s:%d" % (f, line.LineNumber)
    print("  +0x%06X  %s%s" % (off, name, where))
