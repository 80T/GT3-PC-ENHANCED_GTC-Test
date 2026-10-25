"""msvc_layout.py - the MSVC layout of the port's types against the PS2 (egcs) one.

The headers document the layout SCE ee-gcc 2.9-ee-991111b (egcs) gave each
type: member offsets in comments (`u32 x;  /* 0x14 ... */`) and sizes through
EGCS_SIZE(T, n) / EGCS_SIZE_ABI(T, n) / static_assert(sizeof(T) == n), or a
"Size 0xNN" in the comment right before the definition.  The decomp's
tools/hdrcheck.py checks them against egcs; this checks them against MSVC as
the port compiles them, so every type whose MSVC layout moved shows up,
polymorphic ones (EGCS_SIZE skips them) and same-size offset shifts included.

For each header of include/** and src/{game,engine}/** (not the native
folders), one probe translation unit is generated under
build/scratch/layout/probe/: it includes the header (private and protected
made public) with the game's options (pc_prelude.h forced in, /permissive,
-I include -I src, C++17) and reports offsetof / sizeof /
std::is_polymorphic of every documented member and size, and the offset of
every base subobject (egcs: declaration order, the first base at 0, unless
the comment says otherwise; MSVC moves a polymorphic base first and puts a
vptr the class introduces before a plain base), through a callback.
Probe lines that do not compile (a member offsetof cannot reach, a name the
parser got wrong) are dropped and listed; a header that does not compile
alone is listed.  The probes link into probe.exe (unresolved symbols are
forced: nothing but the probes runs), which prints the table.

    python tools/msvc_layout.py [--jobs N] [--only PATTERN] [--all]

Writes build/scratch/layout/report.txt (the types whose MSVC layout differs,
the types MSVC makes larger - the pools and buffers sized for egcs objects -
then the headers / probe lines that could not be checked) and layout.json
(every probed value).  --all also lists the types that agree.  What the
differences mean (which ones matter) is docs/layout.md.

The parsing (comment masking, class bodies, member offsets from comments) is
the decomp's tools/inventory.py / hdrcheck.py, copied here (the decomp is not
imported: the port must not depend on its tree).
"""
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "scratch", "layout")
PROBE = os.path.join(OUT, "probe")
INC = os.path.join(ROOT, "include")
SRC = os.path.join(ROOT, "src")

# The options of the game and engine translation units (src/CMakeLists.txt,
# GT3_DECOMP_OPTIONS, and the top level's definitions).
CL_FLAGS = ["/nologo", "/c", "/std:c++17", "/EHsc", "/MT", "/Od", "/utf-8", "/W0", "/bigobj", "/permissive",
            "/Zc:threadSafeInit-", "/FI" + os.path.join(INC, "pc_prelude.h").replace("\\", "/"),
            "/DWIN32", "/D_WINDOWS", "/DGT3_PC=1", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
            "/D_CRT_NONSTDC_NO_WARNINGS", "/D_CRT_SECURE_NO_WARNINGS",
            "/D_ALLOW_KEYWORD_MACROS",              # the probe's `#define private public`
            "/I" + INC, "/I" + SRC]


# ------------------------------------------------------------------ parsing (from the decomp's inventory.py)
def mask_comments(text):
    """The text with comments and string/char literals replaced by spaces (same
    length), so braces and keywords can be found by position."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                if j < n and text[j] == "\n":
                    break
                j += 1
            for k in range(i + 1, min(j, n)):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def mask_preprocessor(masked):
    """Blank the preprocessor lines too (an `#if 0` block stays: the compiler
    decides; only the directives themselves would confuse the brace scan)."""
    return re.sub(r"(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", lambda m: re.sub(r"[^\n]", " ", m.group(0)), masked)


DEF_RE = re.compile(r"\b(class|struct|union)\s+(?:__declspec\s*\([^)]*\)\s*|alignas\s*\([^)]*\)\s*)*"
                    r"([A-Za-z_]\w*(?:::\w+)*)\s*(?:final\s*)?(:\s*[^{;()]+)?\{")
TYPEDEF_ANON_RE = re.compile(r"\btypedef\s+(struct|union)\s*(?:__declspec\s*\([^)]*\)\s*|alignas\s*\([^)]*\)\s*)*\{")
NS_RE = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)\s*\{")
EXTERN_C_RE = re.compile(r"\bextern\s+\"\s*\"\s*\{")
OFF_RE = re.compile(r"(?:\+|@)?\s*0x([0-9A-Fa-f]{1,5})\b")


def match_brace(masked, i):
    """Index just past the brace block starting at masked[i] == '{'."""
    d = 0
    n = len(masked)
    while i < n:
        c = masked[i]
        if c == "{":
            d += 1
        elif c == "}":
            d -= 1
            if d == 0:
                return i + 1
        i += 1
    return n


def split_top(s):
    """s split at its top-level commas (not inside <>, () or [])."""
    out, d, start = [], 0, 0
    for i, c in enumerate(s):
        if c in "<([":
            d += 1
        elif c in ">)]":
            d -= 1
        elif c == "," and d == 0:
            out.append(s[start:i])
            start = i + 1
    out.append(s[start:])
    return out


def first_declarator(s):
    """`f32 x, y, z` -> `f32 x`: the documented offset is the first one's
    (commas inside <>, () and [] do not split)."""
    d = 0
    for i, c in enumerate(s):
        if c in "<([":
            d += 1
        elif c in ">)]":
            d -= 1
        elif c == "," and d == 0:
            return s[:i].strip()
    return s


def parse_body(raw, masked, lo, hi):
    """Top-level data members of a class body raw[lo:hi] (inside the braces),
    with the offset their comment documents."""
    fields = []
    i = lo
    start = lo
    while i < hi:
        c = masked[i]
        if c == "{":
            # a method body or a nested type: skip it
            j = match_brace(masked, i)
            i = j
            start = i
            continue
        elif c == ";":
            stmt_m = masked[start:i]
            eol = raw.find("\n", i)
            eol = hi if eol < 0 else min(eol, hi)
            tail = raw[i + 1:eol]
            if re.match(r"\s*/\*", tail) and "*/" not in tail:
                close = raw.find("*/", i)               # a comment continued on the next lines
                if 0 <= close < hi:
                    tail = raw[i + 1:close + 2]
            line_start = raw.rfind("\n", 0, i) + 1
            line_text = raw[line_start:eol]
            s = re.sub(r"^(\s*(public|private|protected)\s*:)+", "", stmt_m).strip()
            if s and not s.startswith(("friend", "using", "typedef", "enum", "static_assert", "template", "EGCS_SIZE")):
                s1 = re.sub(r"\s+", " ", s)
                for kw in ("public:", "private:", "protected:"):
                    s1 = s1.replace(kw, "")
                s1 = first_declarator(s1.strip())
                if "(" in s1 and not re.match(r"^[\w:<>\s\*&]+\(\s*\*\s*\w+\s*\)\s*\(", s1):
                    pass                                # a method
                else:
                    m = re.match(r"^(?P<type>.+?)\s*(?P<ptr>[\*&]*)\s*(?P<name>\w+)\s*(?P<arr>(\[[^\]]*\])*)\s*(?P<bits>:\s*\w+)?$", s1)
                    fp = re.match(r"^(?P<type>.+?)\(\s*\*\s*(?P<name>\w+)\s*(?P<arr>(\[[^\]]*\])*)\s*\)\s*\(.*\)$", s1)
                    if fp:
                        m = fp
                    if m and not s1.startswith(("static ", "mutable static")) and " static " not in " " + s1[:20]:
                        com = ""
                        mc = re.match(r"\s*(?:/\*(.*?)\*/|//(.*)$)", tail, re.S) or re.search(r"/\*(.*?)\*/|//(.*)$", line_text)
                        if mc:
                            com = (mc.group(1) or mc.group(2) or "").strip()
                        off = None
                        mo = OFF_RE.search(com)         # the first offset-like number of the comment
                        if mo:
                            off = int(mo.group(1), 16)
                        fields.append({"type": (m.group("type") + ("(*)" if fp else m.group("ptr"))).strip(),
                                       "name": m.group("name"), "arr": m.group("arr") or "", "off": off,
                                       "comment": com[:120], "bits": bool(m.groupdict().get("bits"))})
            start = i + 1
        i += 1
    return fields


def preceding_comment(raw, pos):
    """The block comment right before raw[pos] (only blanks between), or ''."""
    before = raw[:pos].rstrip()
    # a `template <...>` or `typedef` keyword between them is fine
    before = re.sub(r"(typedef|template\s*<[^>]*>)\s*$", "", before).rstrip()
    if not before.endswith("*/"):
        return ""
    k = before.rfind("/*")
    return before[k:] if k >= 0 else ""


DOC_SIZE_RE = re.compile(r"\b[Ss]ize:?\s+(0x[0-9A-Fa-f]+)\b(?!\s*(?:bytes? )?(?:of|per|each|records?|entries|elements))")


def parse_header(path):
    """[{name, line, poly_hint, fields: [(member, off)], size: (n, source) | None}]
    of the classes / structs / unions defined in the header, with qualified names."""
    raw = open(path, encoding="utf-8", errors="replace").read()
    masked = mask_preprocessor(mask_comments(raw))
    # brace depth before each position
    depth = [0] * (len(masked) + 1)
    d = 0
    for i, c in enumerate(masked):
        depth[i] = d
        if c == "{":
            d += 1
        elif c == "}":
            d -= 1
    depth[len(masked)] = d
    spans = []          # (open brace, end, kind, name)
    for m in NS_RE.finditer(masked):
        b = m.end() - 1
        spans.append((b, match_brace(masked, b), "ns", m.group(1), m))
    for m in EXTERN_C_RE.finditer(masked):
        b = m.end() - 1
        spans.append((b, match_brace(masked, b), "extern", "", m))
    for m in DEF_RE.finditer(masked):
        b = m.end() - 1
        pre = masked[max(0, m.start() - 300):m.start()]
        tmpl = bool(re.search(r"template\s*<[^{};]*>\s*$", pre))
        spans.append((b, match_brace(masked, b), "tclass" if tmpl else "class", m.group(2), m))
    for m in TYPEDEF_ANON_RE.finditer(masked):
        b = m.end() - 1
        e = match_brace(masked, b)
        mm = re.match(r"\s*(?:__attribute__\s*\(\(.*?\)\)\s*)?(\w+)\s*[;,\[]", masked[e:e + 200])
        if mm:
            spans.append((b, e, "class", mm.group(1), m))
    spans.sort(key=lambda s: s[0])
    types = []
    sizes = {}
    for (b, e, kind, name, m) in spans:
        if kind not in ("class",):
            continue
        enclosing = [s for s in spans if s[0] < b < s[1]]
        if len(enclosing) != depth[b]:
            continue                            # in a function body, an anonymous struct...
        if any(s[2] == "tclass" for s in enclosing):
            continue
        q = "::".join([s[3] for s in enclosing if s[2] in ("ns", "class")] + [name])
        fields = [(f["name"], f["off"]) for f in parse_body(raw, masked, b + 1, e - 1)
                  if f["off"] is not None and not f["bits"] and not f["name"].startswith("__")]
        com = preceding_comment(raw, m.start())
        doc = DOC_SIZE_RE.search(com)
        line = raw.count("\n", 0, m.start()) + 1
        bases = []
        if m.re is DEF_RE and m.group(3):
            for k, part in enumerate(split_top(m.group(3)[1:])):
                part = re.sub(r"\b(public|private|protected|virtual)\b", "", part).strip()
                if not part:
                    continue
                # the egcs offset of the base when the comment gives it
                # ("Bases: Drawable at +0x00, List<Button> at +0xB0"); the
                # first base is at 0 under egcs anyway
                short = re.escape(part.split("::")[-1])
                mo = re.search(r"\b%s\s*(?:\([^)]*\)\s*)?(?:at\s*)?\+\s*(0x[0-9A-Fa-f]+)" % short, com)
                bases.append((part, int(mo.group(1), 16) if mo else (0 if k == 0 else None)))
        types.append({"name": q, "line": line, "fields": fields, "bases": bases,
                      "doc_size": int(doc.group(1), 16) if doc else None,
                      "ns": [s[3] for s in enclosing if s[2] == "ns"]})
    # documented sizes, qualified by the namespaces around them
    ns_spans = [s for s in spans if s[2] == "ns"]
    size_res = [(r"\bEGCS_SIZE\s*\(\s*([\w:<>, ]+?)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", "EGCS_SIZE"),
                (r"\bEGCS_SIZE_ABI\s*\(\s*([\w:<>, ]+?)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", "EGCS_SIZE_ABI"),
                (r"static_assert\s*\(\s*sizeof\s*\(\s*([\w:<>, ]+?)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)", "static_assert")]
    for rx, src in size_res:
        for m in re.finditer(rx, masked):
            t = m.group(1).strip()
            if t == "T" or masked[max(0, m.start() - 8):m.start()].rstrip().endswith("define"):
                continue
            ns = [s[3] for s in ns_spans if s[0] < m.start() < s[1]]
            q = t.lstrip(":")
            if ns and not t.startswith("::") and not t.startswith("::".join(ns) + "::"):
                q = "::".join(ns + [t])
            sizes[q] = (int(m.group(2), 0), src)
    return types, sizes


# ------------------------------------------------------------------ headers
def header_list(only=None):
    hs = []
    for base in (INC, os.path.join(SRC, "game"), os.path.join(SRC, "engine")):
        for d, dirs, fs in os.walk(base):
            dirs[:] = [x for x in dirs if x != "native"]
            for f in sorted(fs):
                if f.endswith(".h"):
                    p = os.path.join(d, f)
                    if only and not re.search(only, p.replace("\\", "/")):
                        continue
                    hs.append(p)
    hs.sort()
    return hs


def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


# ------------------------------------------------------------------ probes
def probe_entries(path):
    """[(type, member or '#size', documented value, source)]"""
    types, sizes = parse_header(path)
    ents = []
    seen = set()
    known = {t["name"] for t in types}
    for t in types:
        for f, off in t["fields"]:
            if (t["name"], f) not in seen:
                seen.add((t["name"], f))
                ents.append((t["name"], f, off, "comment"))
        # the base subobjects: egcs lays them out in declaration order, the
        # first at 0 (a vptr goes after the data); MSVC moves a polymorphic
        # base first, and puts the vptr a class introduces before a plain base
        for k, (b, off) in enumerate(t["bases"]):
            ents.append((t["name"], "#base:" + b, -1 if off is None else off, "base%d|%s" % (k, "::".join(t["ns"]))))
        if t["name"] in sizes:
            n, src = sizes[t["name"]]
            ents.append((t["name"], "#size", n, src))
        elif t["doc_size"] is not None:
            ents.append((t["name"], "#size", t["doc_size"], "doc"))
        elif t["fields"]:
            ents.append((t["name"], "#size", -1, "none"))     # measured only
    for q, (n, src) in sizes.items():
        if q not in known:
            ents.append((q, "#size", n, src))
    return ents


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_probe(idx, path, ents, dropped):
    src = os.path.join(PROBE, "p%04d.cpp" % idx)
    L = ["/* generated by tools/msvc_layout.py for %s */" % rel(path),
         "#define private public",
         "#define protected public",
         '#include "%s"' % path.replace("\\", "/"),
         "#undef private",
         "#undef protected",
         "#include <stddef.h>",
         "#include <type_traits>",
         "typedef void (*LayoutEmit)(const char *, const char *, int, unsigned, int, const char *);",
         'extern "C" void layout_probe_%04d(LayoutEmit e)' % idx,
         "{"]
    first = len(L) + 1
    lines = {}
    for k, (t, m, doc, how) in enumerate(ents):
        if k in dropped:
            L.append("    /* dropped */")
            continue
        poly = "(int)std::is_polymorphic< %s >::value" % t
        if m == "#size":
            L.append("    e(%s, %s, %d, (unsigned)sizeof(%s), %s, %s);" % (cstr(t), cstr(m), doc, t, poly, cstr(how)))
        elif m.startswith("#base:"):
            # the base's name as written resolves in the class's namespaces
            how, ns = how.split("|", 1)
            parts = ns.split("::") if ns else []
            using = "".join("using namespace %s; " % "::".join(parts[:i + 1]) for i in range(len(parts)))
            L.append("    { %se(%s, %s, %d, (unsigned)((char *)static_cast< %s * >((%s *)0x1000) - (char *)0x1000), %s, %s); }"
                     % (using, cstr(t), cstr(m), doc, m[6:], t, poly, cstr(how)))
        else:
            L.append("    e(%s, %s, %d, (unsigned)offsetof(%s, %s), %s, %s);" % (cstr(t), cstr(m), doc, t, m, poly, cstr(how)))
        lines[first + k] = k
    L.append("}")
    open(src, "w", newline="\n").write("\n".join(L) + "\n")
    return src, first


def msvc_env():
    cache = os.path.join(OUT, "msvc_env.json")
    if os.path.isfile(cache):
        return json.load(open(cache))
    r = subprocess.run(["cmd", "/c", os.path.join(ROOT, "tools", "msvc.cmd"), "set"], capture_output=True, text=True)
    env = {}
    for L in r.stdout.splitlines():
        if "=" in L:
            k, v = L.split("=", 1)
            env[k] = v
    if "INCLUDE" not in env:
        sys.exit("msvc_layout: tools\\msvc.cmd set gave no MSVC environment")
    json.dump(env, open(cache, "w"))
    return env


def tool(env, name):
    """The full path of an MSVC tool (CreateProcess searches our own PATH, not env's)."""
    path = next(v for k, v in env.items() if k.upper() == "PATH")
    p = shutil.which(name, path=path)
    if not p:
        sys.exit("msvc_layout: %s not found in the MSVC environment" % name)
    return p


def compile_one(env, idx, path, ents):
    """Compile a header's probe, dropping the lines that fail; (obj or None, dropped, header error)."""
    dropped = set()
    obj = os.path.join(PROBE, "p%04d.obj" % idx)
    for attempt in range(40):
        src, first = write_probe(idx, path, ents, dropped)
        if os.path.exists(obj):
            os.remove(obj)
        r = subprocess.run([tool(env, "cl")] + CL_FLAGS + ["/Fo" + obj, src], capture_output=True, text=True, env=env,
                           cwd=PROBE)
        out = r.stdout + r.stderr
        if r.returncode == 0 and os.path.isfile(obj):
            return obj, dropped, None
        bad = set()
        other = []
        for L in out.splitlines():
            m = re.match(r"^(.*?)\((\d+)(?:,\d+)?\)\s*:\s*(?:fatal )?error", L)
            if not m:
                continue
            if os.path.normcase(os.path.abspath(m.group(1))) == os.path.normcase(src):
                k = int(m.group(2)) - first
                if 0 <= k < len(ents):
                    bad.add(k)
                else:
                    other.append(L)
            else:
                other.append(L)
        if other and not bad:
            return None, dropped, "\n".join(other[:6])
        if not bad:
            return None, dropped, out.strip()[-1500:]
        dropped |= bad
        if len(dropped) == len(ents):
            return None, dropped, None
    return None, dropped, "gave up"


MAIN_CPP = r"""/* generated by tools/msvc_layout.py */
#include <stdio.h>
typedef void (*LayoutEmit)(const char *, const char *, int, unsigned, int, const char *);
%(decls)s
static int s_hdr;
static void emit(const char *t, const char *m, int doc, unsigned act, int poly, const char *how)
{
    printf("%%d\t%%s\t%%s\t%%d\t%%u\t%%d\t%%s\n", s_hdr, t, m, doc, act, poly, how);
}
int main()
{
%(calls)s
    return 0;
}
"""


def main():
    args = sys.argv[1:]
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else max(2, (os.cpu_count() or 4) - 2)
    only = args[args.index("--only") + 1] if "--only" in args else None
    show_all = "--all" in args
    os.makedirs(PROBE, exist_ok=True)
    env = dict(os.environ)
    env.update(msvc_env())

    hdrs = header_list(only)
    work = []
    for i, h in enumerate(hdrs):
        ents = probe_entries(h)
        if ents:
            work.append((i, h, ents))
    print("headers %d, with documented layout %d, probe lines %d" % (len(hdrs), len(work), sum(len(w[2]) for w in work)))

    results = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(compile_one, env, i, h, ents): (i, h, ents) for (i, h, ents) in work}
        for n, f in enumerate(concurrent.futures.as_completed(futs)):
            i, h, ents = futs[f]
            results[i] = f.result()
            if (n + 1) % 50 == 0:
                print("  compiled %d / %d" % (n + 1, len(work)))

    objs, decls, calls = [], [], []
    for (i, h, ents) in work:
        obj, dropped, err = results[i]
        if obj:
            objs.append(obj)
            decls.append('extern "C" void layout_probe_%04d(LayoutEmit);' % i)
            calls.append("    s_hdr = %d; layout_probe_%04d(emit);" % (i, i))
    mainsrc = os.path.join(PROBE, "main.cpp")
    open(mainsrc, "w", newline="\n").write(MAIN_CPP % {"decls": "\n".join(decls), "calls": "\n".join(calls)})
    mainobj = os.path.join(PROBE, "main.obj")
    r = subprocess.run([tool(env, "cl"), "/nologo", "/c", "/MT", "/Fo" + mainobj, mainsrc], capture_output=True, text=True,
                       env=env, cwd=PROBE)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    exe = os.path.join(PROBE, "probe.exe")
    rsp = os.path.join(PROBE, "link.rsp")
    open(rsp, "w").write("\n".join('"%s"' % o for o in [mainobj] + objs) + "\n")
    r = subprocess.run([tool(env, "link"), "/nologo", "/OUT:" + exe, "/SUBSYSTEM:CONSOLE", "/FORCE:UNRESOLVED",
                        "/FORCE:MULTIPLE", "/IGNORE:4006,4088,4217,4049", "@" + rsp], capture_output=True, text=True,
                       env=env, cwd=PROBE)
    if not os.path.isfile(exe):
        sys.exit("link failed:\n" + (r.stdout + r.stderr)[-3000:])
    r = subprocess.run([exe], capture_output=True, text=True)
    if r.returncode:
        sys.exit("probe.exe failed (%d):\n%s" % (r.returncode, r.stderr[-2000:]))

    # ---------------------------------------------------------------- the table
    rows = []
    for L in r.stdout.splitlines():
        p = L.split("\t")
        if len(p) != 7:
            continue
        rows.append({"hdr": rel(hdrs[int(p[0])]), "type": p[1], "member": p[2], "ps2": int(p[3]),
                     "msvc": int(p[4]), "poly": int(p[5]), "how": p[6]})
    bytype = {}
    for row in rows:
        bytype.setdefault((row["hdr"], row["type"]), []).append(row)
    # a base whose egcs offset the header does not give: right after the
    # previous base (egcs: declaration order), when that one's PS2 size is known
    ps2size = {}
    for row in rows:
        if row["member"] == "#size" and row["ps2"] >= 0:
            ps2size[row["type"]] = row["ps2"]
            ps2size.setdefault(re.sub(r"<.*", "", row["type"]).split("::")[-1], row["ps2"])
    for (hdr, t), rs in bytype.items():
        prev = None
        for x in rs:
            if not x["member"].startswith("#base:"):
                continue
            if x["ps2"] < 0 and prev is not None:
                b = prev["member"][6:]
                n = ps2size.get(b, ps2size.get(re.sub(r"<.*", "", b).split("::")[-1]))
                if n is not None and prev["ps2"] >= 0:
                    x["ps2"] = (prev["ps2"] + n + 3) & ~3
                    x["how"] += " (after the previous base)"
            prev = x
    diff, same = [], []
    for (hdr, t), rs in bytype.items():
        moved = [x for x in rs if x["member"] != "#size" and x["ps2"] >= 0 and x["ps2"] != x["msvc"]]
        sz = [x for x in rs if x["member"] == "#size"]
        szd = [x for x in sz if x["ps2"] >= 0 and x["ps2"] != x["msvc"]]
        rec = {"hdr": hdr, "type": t, "poly": rs[0]["poly"], "moved": moved, "size": sz[0] if sz else None,
               "size_differs": bool(szd), "members": len([x for x in rs if x["member"] != "#size"])}
        (diff if moved or szd else same).append(rec)
    diff.sort(key=lambda x: (x["hdr"], x["type"]))
    json.dump({"rows": rows}, open(os.path.join(OUT, "layout.json"), "w"), indent=0)

    R = ["# MSVC layout against the documented PS2 (egcs) layout - tools/msvc_layout.py", "",
         "headers probed %d (with documented offsets / sizes); types %d; members %d; base subobjects %d; "
         "types differing %d" % (len(objs), len(bytype), sum(1 for x in rows if not x["member"].startswith("#")),
                                 sum(1 for x in rows if x["member"].startswith("#base:")), len(diff)), ""]
    for rec in diff:
        s = rec["size"]
        sz = ""
        if s:
            sz = "size PS2 %s MSVC 0x%X (%s)" % ("0x%X" % s["ps2"] if s["ps2"] >= 0 else "?", s["msvc"], s["how"])
        R.append("%s  %s%s  %s" % (rec["type"], "[polymorphic] " if rec["poly"] else "", rec["hdr"], sz))
        for x in rec["moved"]:
            R.append("    %-28s PS2 0x%-5X MSVC 0x%-5X (%+d)" % (x["member"], x["ps2"], x["msvc"], x["msvc"] - x["ps2"]))
    # pools and fixed buffers are sized for egcs objects: what MSVC makes larger
    R += ["", "## larger under MSVC (check the pools / buffers that hold them)", ""]
    for x in sorted((x for x in rows if x["member"] == "#size" and 0 <= x["ps2"] < x["msvc"]),
                    key=lambda x: (x["hdr"], x["type"])):
        R.append("%-40s PS2 0x%-6X MSVC 0x%-6X (+%d) %s%s" % (x["type"], x["ps2"], x["msvc"], x["msvc"] - x["ps2"],
                                                         "[polymorphic] " if x["poly"] else "", x["hdr"]))
    if show_all:
        R += ["", "## agreeing types", ""]
        for rec in sorted(same, key=lambda x: (x["hdr"], x["type"])):
            s = rec["size"]
            R.append("%s %s%s  %d members%s" % (rec["type"], "[polymorphic] " if rec["poly"] else "", rec["hdr"],
                                                 rec["members"], ", size 0x%X" % s["msvc"] if s else ""))
    R += ["", "## not checked", ""]
    for (i, h, ents) in work:
        obj, dropped, err = results[i]
        if not obj:
            R.append("header %s: does not compile alone%s" % (rel(h), (":\n    " + err.replace("\n", "\n    ")) if err else ""))
        elif dropped:
            R.append("%s: %d probe line(s) dropped: %s" % (rel(h), len(dropped), ", ".join(
                "%s.%s" % (ents[k][0], ents[k][1]) for k in sorted(dropped))[:600]))
    open(os.path.join(OUT, "report.txt"), "w", newline="\n").write("\n".join(R) + "\n")
    print("types %d, differing %d -> %s" % (len(bytype), len(diff), rel(os.path.join(OUT, "report.txt"))))


if __name__ == "__main__":
    main()
