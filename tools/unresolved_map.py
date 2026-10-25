#!/usr/bin/env python3
"""unresolved_map.py - group the link's unresolved symbols by the decomp
file that defines them (via the `// FUNC 0x... name` markers and global
definitions of decomp/core_gt3enhanced).

  python tools/unresolved_map.py build/link_log.txt [--files]
"""
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.normpath(os.path.join(HERE, ".."))
DECOMP = os.path.normpath(os.path.join(PORT, "..", "decomp", "core_gt3enhanced"))


def readable(sym):
    """MSVC prints `"readable" (decorated)` for C++ symbols, `_name` for C."""
    m = re.match(r'"(.*)" \((.*)\)$', sym)
    if m:
        s = m.group(1)
        # strip return type / calling convention / params: keep qualified name
        m2 = re.search(r"([\w:~<>,\s\*&]+?)\(", s)
        name = s
        mm = re.search(r"(?:__cdecl|__thiscall|__stdcall)\s+(.+?)\(", s)
        if mm:
            name = mm.group(1)
        else:
            parts = s.split()
            name = parts[-1] if parts else s
        return name.strip()
    return sym.lstrip("_")


def main():
    log = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    syms = set(re.findall(r"LNK2019: unresolved external symbol (.*?) referenced", log))
    syms |= set(m.strip() for m in re.findall(r"LNK2001: unresolved external symbol (.*?)\r?$", log, re.M))
    syms = sorted(syms)
    index = {}
    marker = re.compile(r"^// FUNC 0x[0-9A-Fa-f]{8} (\S+)")
    for tree in ("src/engine", "src/sdk", "src/runtime", "src/game"):
        for dp, dn, fn in os.walk(os.path.join(DECOMP, tree)):
            for n in fn:
                if not n.endswith((".c", ".cpp", ".S", ".inc")):
                    continue
                p = os.path.join(dp, n)
                rel = os.path.relpath(p, DECOMP).replace("\\", "/")
                for line in open(p, encoding="utf-8", errors="replace"):
                    m = marker.match(line)
                    if m:
                        name = m.group(1)
                        index.setdefault(name, rel)
                        index.setdefault(name.split("::")[-1], rel)
                    else:
                        m = re.match(r"^(?:const\s+|static\s+|volatile\s+)*[A-Za-z_][\w:<>\*\s]*?\b(g_[0-9A-Fa-f]{8}|[A-Za-z_]\w*)\s*(\[[^\]]*\])*\s*(=|;)", line)
                        if m and not line.startswith(("extern", "typedef", "#", " ", "\t", "return")):
                            index.setdefault(m.group(1), rel)
    groups = defaultdict(list)
    for s in syms:
        name = readable(s)
        key = name
        f = index.get(key) or index.get(key.split("::")[-1]) or "?"
        groups[f].append(name)
    for f in sorted(groups, key=lambda k: (-len(groups[k]), k)):
        print("%4d %s" % (len(groups[f]), f))
        if "--files" not in sys.argv:
            for n in groups[f]:
                print("       " + n)


if __name__ == "__main__":
    main()
