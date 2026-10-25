#!/usr/bin/env python3
"""gt3vol.py - list and extract files of Gran Turismo 3's gt3.vol ("RoFS").

The layout is the one engine/io/T500_vol_toc.cpp and T510_vol_device.cpp read:
the TOC sits at the start of the volume (header word 3 = header size, word 4 =
body size; a TOC stored with the magic ~"RoFS" has its body bytes inverted).
Directory record at offset `off`: +0 name offset (low 24 bits), +4 entry count
+ 1, +8 parent record, +0xC entry record offsets.  Root record 0x14.  Entry
record: +3 type (1 directory, 2 gzip-packed file, else a plain file), +4 first
sector (relative to the volume, 0x800 bytes), +8 size, +0xC packed size.

  python tools/gt3vol.py list [--vol PATH] [PREFIX]
  python tools/gt3vol.py extract [--vol PATH] [--raw] PATH_IN_VOL OUT_FILE
  python tools/gt3vol.py extract-all [--vol PATH] [--raw] OUT_DIR [PREFIX]

Packed files are inflated unless --raw is given.  The default volume is
pc_enhanced/gt3_disc/files/gt3.vol next to this script's folder.
"""
import argparse
import os
import struct
import sys
import zlib

SECTOR = 0x800
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_VOL = os.path.join(HERE, "..", "gt3_disc", "files", "gt3.vol")


class Volume:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(0x40)
        words = struct.unpack_from("<16I", hdr)
        size = words[3] + words[4]
        self.f.seek(0)
        toc = bytearray(self.f.read((size + SECTOR - 1) // SECTOR * SECTOR))
        if toc[:4] == b"\xAD\x90\xB9\xAC":
            start, body = words[3], words[4]
            for i in range(start, start + body):
                toc[i] ^= 0xFF
            toc[:4] = b"RoFS"
        if toc[:4] != b"RoFS":
            raise ValueError("%s: not a RoFS volume" % path)
        self.toc = bytes(toc)

    def word(self, off):
        return struct.unpack_from("<I", self.toc, off)[0]

    def name(self, rec):
        off = self.word(rec) & 0xFFFFFF
        end = self.toc.index(b"\0", off)
        return self.toc[off:end].decode("latin-1")

    def walk(self, rec=0x14, prefix=""):
        """Yields (path, entry record offset, type, lsn, size, packed size)."""
        count = self.word(rec + 4) - 1
        for i in range(count):
            ent = self.word(rec + 0xC + 4 * i)
            name = self.name(ent)
            typ = self.toc[ent + 3]
            path = prefix + name
            if typ == 1:
                yield path + "/", ent, typ, 0, 0, 0
                yield from self.walk(ent, path + "/")
            else:
                lsn, size, packed = struct.unpack_from("<3I", self.toc, ent + 4)
                yield path, ent, typ, lsn, size, packed

    def lookup(self, path):
        for p, ent, typ, lsn, size, packed in self.walk():
            if p == path.lstrip("/"):
                return ent, typ, lsn, size, packed
        raise KeyError(path)

    def read(self, path, raw=False):
        ent, typ, lsn, size, packed = self.lookup(path)
        if typ == 1:
            raise IsADirectoryError(path)
        self.f.seek(lsn * SECTOR)
        if typ == 2:
            data = self.f.read(packed)
            if raw:
                return data
            return zlib.decompress(data, 16 + zlib.MAX_WBITS)[:size]
        return self.f.read(size)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vol", default=DEFAULT_VOL)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("prefix", nargs="?", default="")
    p = sub.add_parser("extract")
    p.add_argument("--raw", action="store_true")
    p.add_argument("path")
    p.add_argument("out")
    p = sub.add_parser("extract-all")
    p.add_argument("--raw", action="store_true")
    p.add_argument("outdir")
    p.add_argument("prefix", nargs="?", default="")
    args = ap.parse_args()

    vol = Volume(args.vol)
    if args.cmd == "list":
        for path, ent, typ, lsn, size, packed in vol.walk():
            if not path.startswith(args.prefix):
                continue
            if typ == 1:
                print("%-60s <dir>" % path)
            else:
                kind = "gz" if typ == 2 else "  "
                print("%-60s %s lsn %8d size %10d packed %10d" % (path, kind, lsn, size, packed))
    elif args.cmd == "extract":
        data = vol.read(args.path, args.raw)
        with open(args.out, "wb") as f:
            f.write(data)
        print("%s: %d bytes" % (args.out, len(data)))
    else:
        n = 0
        for path, ent, typ, lsn, size, packed in vol.walk():
            if typ == 1 or not path.startswith(args.prefix):
                continue
            out = os.path.join(args.outdir, *path.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(vol.read(path, args.raw))
            n += 1
        print("%d files" % n)


if __name__ == "__main__":
    sys.exit(main())
