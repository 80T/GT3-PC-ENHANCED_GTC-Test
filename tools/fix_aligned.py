#!/usr/bin/env python3
"""fix_aligned.py - rewrite GNU `__attribute__((aligned(N)))` on variable and
member declarations as C++ `alignas(N)` (MSVC ignores the GNU spelling: the
port's prelude erases it).

  TYPE name[...] __attribute__((aligned(N))) [= ...|;]
  -> alignas(N) TYPE name[...] [= ...|;]

Struct-level attributes (`} __attribute__((aligned(N))) Name;`), packed
structs and macros are left for hand edits and listed.

  python tools/fix_aligned.py [paths...]   (default: src/game src/engine include)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.normpath(os.path.join(HERE, ".."))

DECL = re.compile(r"^(?P<indent>\s*)(?P<decl>(?:static\s+|extern\s+|const\s+|volatile\s+)*[A-Za-z_][\w:<>]*(?:\s*\*)*\s+"
                  r"[A-Za-z_]\w*(?:\s*\[[^\]]*\])*)\s+__attribute__\s*\(\(\s*aligned\s*\(\s*(?P<n>\d+)\s*\)\s*\)\)"
                  r"(?P<rest>\s*(?:=|;).*)$")


def main():
    roots = sys.argv[1:] or ["src/game", "src/engine", "include"]
    changed = 0
    left = []
    for root in roots:
        for dp, dn, fn in os.walk(os.path.join(PORT, root)):
            if os.sep + "native" in dp:
                continue
            for n in fn:
                if not n.endswith((".c", ".cpp", ".h", ".inc")):
                    continue
                p = os.path.join(dp, n)
                lines = open(p, encoding="utf-8", errors="surrogateescape").read().split("\n")
                touched = False
                for i, l in enumerate(lines):
                    if "__attribute__" not in l or "aligned" not in l:
                        continue
                    m = DECL.match(l)
                    if m:
                        lines[i] = "%salignas(%s) %s%s" % (m.group("indent"), m.group("n"), m.group("decl"), m.group("rest"))
                        touched = True
                        changed += 1
                    else:
                        left.append("%s:%d: %s" % (os.path.relpath(p, PORT), i + 1, l.strip()))
                if touched:
                    with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
                        f.write("\n".join(lines))
    print("%d declarations rewritten" % changed)
    if left:
        print("left for hand edits:")
        for l in left:
            print("  " + l)


if __name__ == "__main__":
    main()
