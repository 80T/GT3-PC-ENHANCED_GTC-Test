#!/usr/bin/env python3
"""import_decomp.py - copy the decompiled game into the port tree.

Copies decomp/core_gt3enhanced/{src/game, src/engine, include} into
pc_enhanced/{src/game, src/engine, include} and drops what only the PS2
builds need:

  * the egcs (period compiler) variants: #ifdef BOOT_OLD_GCC,
    #if defined(BOOT_OLD_GCC) && !defined(NON_MATCHING), egcs-only __GNUC__
    tests;
  * the GCC 14 build's hand-written egcs RTTI: every #ifndef BOOT_OLD_GCC
    block (typeinfo functions __tf*, __rtti_* calls, typeinfo storage, the
    explicit __main call).  MSVC generates its own RTTI and static
    constructors.

Every other preprocessor line is left alone.  Each dropped block is listed in
build/import/dropped.txt for review.  The port's own edits come after the
import, so the script refuses to overwrite an existing tree unless --force.

  python tools/import_decomp.py [--force] [--only src/game]
"""
import argparse
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.normpath(os.path.join(HERE, ".."))
DECOMP = os.path.normpath(os.path.join(PORT, "..", "decomp", "core_gt3enhanced"))
TREES = ["src/game", "src/engine", "include"]
SOURCE_EXT = (".c", ".cpp", ".h", ".inc", ".S")

# Normalised condition (comment stripped, spaces collapsed) -> its value in
# the port.  True / False: the condition's value.  "drop": the whole if-part
# is dropped although the condition is true in a non-egcs build (GCC 14 RTTI);
# an #else part is an error (reviewed by hand).
RULES = {
    # The GCC 14 side: dropped when it is egcs RTTI / static-constructor
    # emulation (judged by its content and its comment), kept otherwise
    # (some blocks hold code the GCC 14 build needs, such as a return
    # value egcs leaves in $v0).
    "#ifndef BOOT_OLD_GCC": "drop_if_rtti",
    "#ifdef BOOT_OLD_GCC": False,
    "#if defined(BOOT_OLD_GCC) && !defined(NON_MATCHING)": False,
    "#if defined(BOOT_OLD_GCC)": False,
    "#if defined(__GNUC__) && __GNUC__ < 3": False,
    "#if defined(__cplusplus) && !defined(BOOT_OLD_GCC)": "drop",
    "#if defined(__cplusplus) && defined(BOOT_OLD_GCC)": False,
    "#if !defined(BOOT_OLD_GCC) || !defined(GAME_T153_EMITS_TYPEINFO)": "drop",
    "#if !defined(BOOT_OLD_GCC) || !defined(GAME_T194_EMITS_TYPEINFO)": "drop",
    "#if defined(GAME_T180_INLINE_BLOCK_ACCESSORS) || !defined(BOOT_OLD_GCC)": True,
    "#if defined(BOOT_OLD_GCC) && defined(__NEW__)": False,
    # PS2 runtime headers (include/runtime/cxx): egcs RTTI / <new> support.
    "#if !defined(BOOT_OLD_GCC) || !defined(__NEW__)": "drop",
    "#if !defined(BOOT_OLD_GCC) || !defined(T549_TINFO2_LOCAL_CLASSES)": "drop",
    # GCC version tests that are simply false under MSVC: left in place.
    "#if defined(__GNUC__) && __GNUC__ >= 3": None,
    "#if defined(__GNUC__) && __GNUC__ >= 4": None,
}

DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")


def normalise(line):
    line = re.sub(r"/\*.*?(\*/|$)", "", line)
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r"\s+", " ", line.strip())
    line = re.sub(r"^#\s*", "#", line)
    return line


def mentions_compiler(cond):
    return "BOOT_OLD_GCC" in cond or "__GNUC__" in cond


# Content of a GCC 14 block that only emulates what egcs generated itself:
# typeinfo functions and objects, the RTTI runtime, __main, explicit static
# constructors / destructors and the externs of the objects they build.
RTTI_CODE = re.compile(r"__tf|__ti[0-9A-Z_]|__rtti|type_info|tinfo|base_info|cxxrt\.h|\b__main\b|"
                       r"_static_ctor|_static_dtor|_static_init|_static_destroy|__tcf_|_GLOBAL_\$|typeinfo")
OLD_RULES = False       # --old-rules: the first import's rule (every GCC 14 block dropped)


def block_is_rtti(directive, lines, start):
    """Whether the #ifndef BOOT_OLD_GCC block opened at lines[start] is
    RTTI / static-constructor emulation."""
    if OLD_RULES or "egcs generates" in directive or "egcs's <new>" in directive:
        return True
    depth = 0
    for j in range(start + 1, len(lines)):
        m = DIRECTIVE.match(lines[j])
        if m and m.group(1) in ("if", "ifdef", "ifndef"):
            depth += 1
        elif m and m.group(1) == "endif":
            if depth == 0:
                return False
            depth -= 1
        elif m and m.group(1) == "else" and depth == 0:
            return False
        code = re.sub(r"/\*.*?(\*/|$)", "", lines[j])
        code = re.sub(r"//.*$", "", code)
        if RTTI_CODE.search(code):
            return True
    return False


class ImportError_(Exception):
    pass


def process(path, text, dropped):
    """Returns the text with the known compiler conditionals resolved."""
    lines = text.split("\n")
    out = []
    # Stack entries: [kind, state] where kind is "keep" (an untouched #if)
    # or "rule"; state for "rule": "take" (emit lines), "skip" (drop lines),
    # and whether an #else was seen.
    stack = []

    def emitting():
        return all(e["emit"] for e in stack)

    i = 0
    while i < len(lines):
        line = lines[i]
        m = DIRECTIVE.match(line)
        if m:
            kw = m.group(1)
            if kw in ("if", "ifdef", "ifndef"):
                cond = normalise(line)
                if mentions_compiler(cond):
                    if cond not in RULES:
                        raise ImportError_("%s:%d: unknown compiler conditional: %s" % (path, i + 1, line.strip()))
                    rule = RULES[cond]
                    if rule == "drop_if_rtti":
                        rule = "drop" if block_is_rtti(line, lines, i) else True
                    if rule is None:
                        stack.append({"kind": "keep", "emit": True})
                        if emitting():
                            out.append(line)
                        i += 1
                        continue
                    if rule == "drop" or rule is False:
                        stack.append({"kind": "rule", "emit": False, "drop": rule == "drop", "start": i + 1})
                        if emitting_parent(stack):
                            dropped.append("%s:%d: %s" % (path, i + 1, line.strip()))
                    else:
                        stack.append({"kind": "rule", "emit": True, "drop": False, "start": i + 1})
                    i += 1
                    continue
                stack.append({"kind": "keep", "emit": True})
                if emitting():
                    out.append(line)
                i += 1
                continue
            if kw in ("elif",):
                if stack and stack[-1]["kind"] == "rule":
                    raise ImportError_("%s:%d: #elif inside a resolved conditional" % (path, i + 1))
                if emitting():
                    out.append(line)
                i += 1
                continue
            if kw == "else":
                top = stack[-1]
                if top["kind"] == "rule":
                    if top["drop"]:
                        raise ImportError_("%s:%d: #else after a dropped GCC 14 block (line %d): review by hand"
                                           % (path, i + 1, top["start"]))
                    top["emit"] = not top["emit"]
                    i += 1
                    continue
                if emitting_parent(stack):
                    out.append(line)
                i += 1
                continue
            if kw == "endif":
                top = stack.pop()
                if top["kind"] == "keep" and emitting():
                    out.append(line)
                i += 1
                continue
        if emitting():
            out.append(line)
        i += 1
    if stack:
        raise ImportError_("%s: unbalanced conditionals" % path)
    return "\n".join(out)


def emitting_parent(stack):
    return all(e["emit"] for e in stack[:-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="overwrite an existing tree")
    ap.add_argument("--only", action="append", help="import only this tree (repeatable)")
    ap.add_argument("--out", help="write the import here instead of the port tree (for merges)")
    ap.add_argument("--old-rules", action="store_true", help="the first import's rules (for merges)")
    args = ap.parse_args()

    global OLD_RULES
    OLD_RULES = args.old_rules
    out_root = os.path.abspath(args.out) if args.out else PORT
    trees = args.only or TREES
    for t in trees:
        dst = os.path.join(out_root, t)
        if os.path.exists(dst) and not args.force and not args.out:
            print("error: %s exists (the port edits it); pass --force to overwrite" % dst)
            return 1

    dropped = []
    errors = []
    count = 0
    for t in trees:
        src_root = os.path.join(DECOMP, t)
        dst_root = os.path.join(out_root, t)
        if os.path.exists(dst_root):
            shutil.rmtree(dst_root)
        for dirpath, dirnames, filenames in os.walk(src_root):
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            rel = os.path.relpath(dirpath, src_root)
            os.makedirs(os.path.join(dst_root, rel), exist_ok=True)
            for name in filenames:
                src = os.path.join(dirpath, name)
                dst = os.path.join(dst_root, rel, name)
                if not name.endswith(SOURCE_EXT):
                    shutil.copyfile(src, dst)
                    continue
                with open(src, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    text = f.read()
                relpath = os.path.relpath(src, DECOMP).replace("\\", "/")
                try:
                    text = process(relpath, text.replace("\r\n", "\n"), dropped)
                except ImportError_ as e:
                    errors.append(str(e))
                with open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
                    f.write(text)
                count += 1

    logdir = os.path.join(out_root, "build", "import") if args.out else os.path.join(PORT, "build", "import")
    os.makedirs(logdir, exist_ok=True)
    with open(os.path.join(logdir, "dropped.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(dropped) + "\n")
    print("%d source files imported, %d conditional blocks resolved away" % (count, len(dropped)))
    if errors:
        print("%d errors:" % len(errors))
        for e in errors:
            print("  " + e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
