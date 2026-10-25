#!/usr/bin/env python3
"""vu1dis.py - disassembler for GT3's VU1 microcode (g_gsVu1Microcode).

The microcode is the initialiser of g_gsVu1Microcode in the decomp
(src/engine/pgl/vu1_microcode.inc): 0x3DA8 bytes, XOR-scrambled with the low
byte of successive values of the LCG of gs_driver.c (lcg_rand 0x0025A1B8:
state = state * 0x5D588B65 + 1, initial state 0x32277070); gfx_init
(0x00250820) unscrambles it in place and uploads it to VU1 micro memory 0.

A VU instruction is 64 bits: the lower instruction in bits 0..31, the upper
in bits 32..63.  Addresses below are instruction indices (VIF MSCAL / ITOP
units: byte address / 8).  The opcode tables follow the field layout of the
VU instruction set (upper: OP 0..5, FD 6..10, FS 11..15, FT 16..20, dest
21..24 = w z y x, flags I E M D T in 31..27; lower: OP 25..31).

  python vu1dis.py [--inc path/vu1_microcode.inc] [--bin out.bin] [-o listing.txt]
"""
import argparse
import os
import re
import struct
import sys

DEFAULT_INC = os.path.join(os.path.dirname(__file__), '..', '..', '..', 'decomp',
                           'core_gt3enhanced', 'src', 'engine', 'pgl', 'vu1_microcode.inc')


def load_microcode(inc_path):
    """Bytes of the .inc initialiser, unscrambled."""
    text = open(inc_path, encoding='utf-8').read()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    data = bytearray(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})\b', text))
    state = 0x32277070
    for i in range(len(data)):
        state = (state * 0x5D588B65 + 1) & 0xFFFFFFFF
        data[i] ^= state & 0xFF
    return bytes(data)


def dest(code):
    return ('x' if code & (1 << 24) else '') + ('y' if code & (1 << 23) else '') + \
           ('z' if code & (1 << 22) else '') + ('w' if code & (1 << 21) else '')


FT = lambda c: (c >> 16) & 31
FS = lambda c: (c >> 11) & 31
FD = lambda c: (c >> 6) & 31
BC = 'xyzw'


def vf(n):
    return 'vf%02d' % n


def vi(n):
    return 'vi%02d' % (n & 15)


# ----------------------------------------------------------------------------
# Upper
# ----------------------------------------------------------------------------
def dis_upper(c):
    op = c & 0x3F
    d = dest(c)
    fd, fs, ft = FD(c), FS(c), FT(c)
    if op < 0x1C:
        names = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']
        n = names[op >> 2] + BC[op & 3]
        return '%s.%s %s, %s, %s%s' % (n, d, vf(fd), vf(fs), vf(ft), BC[op & 3])
    qi = {0x1C: ('MULq', 'Q'), 0x1D: ('MAXi', 'I'), 0x1E: ('MULi', 'I'), 0x1F: ('MINIi', 'I'),
          0x20: ('ADDq', 'Q'), 0x21: ('MADDq', 'Q'), 0x22: ('ADDi', 'I'), 0x23: ('MADDi', 'I'),
          0x24: ('SUBq', 'Q'), 0x25: ('MSUBq', 'Q'), 0x26: ('SUBi', 'I'), 0x27: ('MSUBi', 'I')}
    if op in qi:
        n, r = qi[op]
        return '%s.%s %s, %s, %s' % (n, d, vf(fd), vf(fs), r)
    full = {0x28: 'ADD', 0x29: 'MADD', 0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB',
            0x2E: 'OPMSUB', 0x2F: 'MINI'}
    if op in full:
        return '%s.%s %s, %s, %s' % (full[op], d, vf(fd), vf(fs), vf(ft))
    if op >= 0x3C:
        bc = op & 3
        sub = fd
        tab = [
            ['ADDAx', 'SUBAx', 'MADDAx', 'MSUBAx', 'ITOF0', 'FTOI0', 'MULAx', 'MULAq', 'ADDAq', 'SUBAq', 'ADDA', 'SUBA'],
            ['ADDAy', 'SUBAy', 'MADDAy', 'MSUBAy', 'ITOF4', 'FTOI4', 'MULAy', 'ABS', 'MADDAq', 'MSUBAq', 'MADDA', 'MSUBA'],
            ['ADDAz', 'SUBAz', 'MADDAz', 'MSUBAz', 'ITOF12', 'FTOI12', 'MULAz', 'MULAi', 'ADDAi', 'SUBAi', 'MULA', 'OPMULA'],
            ['ADDAw', 'SUBAw', 'MADDAw', 'MSUBAw', 'ITOF15', 'FTOI15', 'MULAw', 'CLIP', 'MADDAi', 'MSUBAi', None, 'NOP'],
        ][bc]
        if sub >= len(tab) or tab[sub] is None:
            return '??upper %08X' % c
        n = tab[sub]
        if n == 'NOP':
            return 'NOP'
        if n == 'CLIP':
            return 'CLIPw.xyz %s, %sw' % (vf(fs), vf(ft))
        if n.startswith('ITOF') or n.startswith('FTOI') or n == 'ABS':
            return '%s.%s %s, %s' % (n, d, vf(ft), vf(fs))
        if n.endswith('q'):
            return '%s.%s ACC, %s, Q' % (n, d, vf(fs))
        if n.endswith('i'):
            return '%s.%s ACC, %s, I' % (n, d, vf(fs))
        if n in ('ADDA', 'SUBA', 'MADDA', 'MSUBA', 'MULA', 'OPMULA'):
            return '%s.%s ACC, %s, %s' % (n, d, vf(fs), vf(ft))
        return '%s.%s ACC, %s, %s%s' % (n, d, vf(fs), vf(ft), BC[bc])
    return '??upper %08X' % c


# ----------------------------------------------------------------------------
# Lower
# ----------------------------------------------------------------------------
def imm11(c):
    v = c & 0x7FF
    return v - 0x800 if v & 0x400 else v


def imm5(c):
    v = (c >> 6) & 0x1F
    return v - 0x20 if v & 0x10 else v


def imm15(c):
    return ((c >> 10) & 0x7800) | (c & 0x7FF)


def imm12(c):
    return (((c >> 21) & 1) << 11) | (c & 0x7FF)


def fsf(c):
    return BC[(c >> 21) & 3]


def ftf(c):
    return BC[(c >> 23) & 3]


def dis_lower(c, pc, labels):
    """Returns (text, branch target or None, is_jump)."""
    op = c >> 25
    d = dest(c)
    it, is_, ft, fs = FT(c), FS(c), FT(c), FS(c)
    tgt = None

    def target():
        return pc + 1 + imm11(c)

    if op == 0x00:
        return 'LQ.%s %s, %d(%s)' % (d, vf(ft), imm11(c), vi(is_)), None, False
    if op == 0x01:
        return 'SQ.%s %s, %d(%s)' % (d, vf(fs), imm11(c), vi(it)), None, False
    if op == 0x04:
        return 'ILW.%s %s, %d(%s)' % (d, vi(it), imm11(c), vi(is_)), None, False
    if op == 0x05:
        return 'ISW.%s %s, %d(%s)' % (d, vi(it), imm11(c), vi(is_)), None, False
    if op == 0x08:
        return 'IADDIU %s, %s, 0x%X' % (vi(it), vi(is_), imm15(c)), None, False
    if op == 0x09:
        return 'ISUBIU %s, %s, 0x%X' % (vi(it), vi(is_), imm15(c)), None, False
    fc = {0x10: 'FCEQ', 0x12: 'FCAND', 0x13: 'FCOR'}
    if op in fc:
        return '%s vi01, 0x%06X' % (fc[op], c & 0xFFFFFF), None, False
    if op == 0x11:
        return 'FCSET 0x%06X' % (c & 0xFFFFFF), None, False
    fsx = {0x14: 'FSEQ', 0x16: 'FSAND', 0x17: 'FSOR'}
    if op in fsx:
        return '%s %s, 0x%03X' % (fsx[op], vi(it), imm12(c)), None, False
    if op == 0x15:
        return 'FSSET 0x%03X' % imm12(c), None, False
    fm = {0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR'}
    if op in fm:
        return '%s %s, %s' % (fm[op], vi(it), vi(is_)), None, False
    if op == 0x1C:
        return 'FCGET %s' % vi(it), None, False
    if op == 0x20:
        t = target()
        return 'B %s' % labels.get(t, '0x%03X' % t), t, True
    if op == 0x21:
        t = target()
        return 'BAL %s, %s' % (vi(it), labels.get(t, '0x%03X' % t)), t, False
    if op == 0x24:
        return 'JR %s' % vi(is_), None, True
    if op == 0x25:
        return 'JALR %s, %s' % (vi(it), vi(is_)), None, False
    br2 = {0x28: 'IBEQ', 0x29: 'IBNE'}
    if op in br2:
        t = target()
        return '%s %s, %s, %s' % (br2[op], vi(it), vi(is_), labels.get(t, '0x%03X' % t)), t, False
    br1 = {0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
    if op in br1:
        t = target()
        return '%s %s, %s' % (br1[op], vi(is_), labels.get(t, '0x%03X' % t)), t, False
    if op == 0x40:
        sub = c & 0x3F
        idd = FD(c)
        if sub == 0x30:
            return 'IADD %s, %s, %s' % (vi(idd), vi(is_), vi(it)), None, False
        if sub == 0x31:
            return 'ISUB %s, %s, %s' % (vi(idd), vi(is_), vi(it)), None, False
        if sub == 0x32:
            return 'IADDI %s, %s, %d' % (vi(it), vi(is_), imm5(c)), None, False
        if sub == 0x34:
            return 'IAND %s, %s, %s' % (vi(idd), vi(is_), vi(it)), None, False
        if sub == 0x35:
            return 'IOR %s, %s, %s' % (vi(idd), vi(is_), vi(it)), None, False
        if sub >= 0x3C:
            k = ((FD(c)) << 2) | (sub & 3)
            # the T3 tables, by (FD, the low 2 bits of the opcode)
            T = {
                (0x0C, 0): 'MOVE', (0x0D, 0): 'LQI', (0x0E, 0): 'DIV', (0x0F, 0): 'MTIR',
                (0x10, 0): 'RNEXT', (0x19, 0): 'MFP', (0x1A, 0): 'XTOP', (0x1B, 0): 'XGKICK',
                (0x1C, 0): 'ESADD', (0x1D, 0): 'EATANxy', (0x1E, 0): 'ESQRT', (0x1F, 0): 'ESIN',
                (0x0C, 1): 'MR32', (0x0D, 1): 'SQI', (0x0E, 1): 'SQRT', (0x0F, 1): 'MFIR',
                (0x10, 1): 'RGET', (0x1A, 1): 'XITOP', (0x1C, 1): 'ERSADD', (0x1D, 1): 'EATANxz',
                (0x1E, 1): 'ERSQRT', (0x1F, 1): 'EATAN',
                (0x0D, 2): 'LQD', (0x0E, 2): 'RSQRT', (0x0F, 2): 'ILWR', (0x10, 2): 'RINIT',
                (0x1C, 2): 'ELENG', (0x1D, 2): 'ESUM', (0x1E, 2): 'ERCPR', (0x1F, 2): 'EEXP',
                (0x0D, 3): 'SQD', (0x0E, 3): 'WAITQ', (0x0F, 3): 'ISWR', (0x10, 3): 'RXOR',
                (0x1C, 3): 'ERLENG', (0x1E, 3): 'WAITP',
            }
            n = T.get((FD(c), sub & 3))
            if n is None:
                return '??lower %08X' % c, None, False
            if n == 'MOVE':
                if fs == 0 and ft == 0 and d == '':
                    return 'NOP', None, False
                return 'MOVE.%s %s, %s' % (d, vf(ft), vf(fs)), None, False
            if n == 'MR32':
                return 'MR32.%s %s, %s' % (d, vf(ft), vf(fs)), None, False
            if n == 'LQI':
                return 'LQI.%s %s, (%s++)' % (d, vf(ft), vi(is_)), None, False
            if n == 'LQD':
                return 'LQD.%s %s, (--%s)' % (d, vf(ft), vi(is_)), None, False
            if n == 'SQI':
                return 'SQI.%s %s, (%s++)' % (d, vf(fs), vi(it)), None, False
            if n == 'SQD':
                return 'SQD.%s %s, (--%s)' % (d, vf(fs), vi(it)), None, False
            if n == 'DIV':
                return 'DIV Q, %s%s, %s%s' % (vf(fs), fsf(c), vf(ft), ftf(c)), None, False
            if n == 'RSQRT':
                return 'RSQRT Q, %s%s, %s%s' % (vf(fs), fsf(c), vf(ft), ftf(c)), None, False
            if n == 'SQRT':
                return 'SQRT Q, %s%s' % (vf(ft), ftf(c)), None, False
            if n == 'WAITQ' or n == 'WAITP':
                return n, None, False
            if n == 'MTIR':
                return 'MTIR %s, %s%s' % (vi(it), vf(fs), fsf(c)), None, False
            if n == 'MFIR':
                return 'MFIR.%s %s, %s' % (d, vf(ft), vi(is_)), None, False
            if n == 'ILWR':
                return 'ILWR.%s %s, (%s)' % (d, vi(it), vi(is_)), None, False
            if n == 'ISWR':
                return 'ISWR.%s %s, (%s)' % (d, vi(it), vi(is_)), None, False
            if n in ('RNEXT', 'RGET'):
                return '%s.%s %s, R' % (n, d, vf(ft)), None, False
            if n in ('RINIT', 'RXOR'):
                return '%s R, %s%s' % (n, vf(fs), fsf(c)), None, False
            if n == 'MFP':
                return 'MFP.%s %s, P' % (d, vf(ft)), None, False
            if n in ('XTOP', 'XITOP'):
                return '%s %s' % (n, vi(it)), None, False
            if n == 'XGKICK':
                return 'XGKICK %s' % vi(is_), None, False
            if n in ('ESADD', 'ERSADD', 'ELENG', 'ERLENG', 'EATANxy', 'EATANxz', 'ESUM'):
                return '%s P, %s' % (n, vf(fs)), None, False
            return '%s P, %s%s' % (n, vf(fs), fsf(c)), None, False
    return '??lower %08X' % c, None, False


# What the model programs keep in a VU1 data buffer (TOPS relative, the
# vi02 base); docs/models.md has the details.
VUMEM = {
    0x00: 'xyz[0]', 0x20: 'xyz2[0] (morph)', 0x3F: 'st-1', 0x40: 'st[0] / strip count (w)', 0x60: 'st2[0] (morph)',
    0x7F: 'rgba-1', 0x80: 'rgba|normal[0]', 0xA0: 'rgba2|normal2[0] (morph)', 0xC0: 'GIF tag',
    0xC1: 'matrix row 0', 0xC2: 'matrix row 1', 0xC3: 'matrix row 2', 0xC4: 'matrix row 3',
    0xC5: 'viewScale', 0xC6: 'viewCenter', 0xC7: 'limits', 0xC8: 'vBB0 (guard band)',
    0xD9: 'GS register packet', 0xF1: 'output GIF tag - 1', 0xF2: 'morph weight (w) / output',
}
for k in range(0xC9, 0xD9):
    VUMEM.setdefault(k, 'light block +%d' % (k - 0xC9))


def annotate(text):
    import re
    m = re.search(r'(-?\d+)\(vi02\)', text)
    if m:
        a = int(m.group(1))
        if a in VUMEM:
            return '; TOPS+0x%02X %s' % (a, VUMEM[a])
    m = re.search(r'IADDIU vi\d\d, vi02, 0x([0-9A-F]+)', text)
    if m:
        a = int(m.group(1), 16)
        if a in VUMEM:
            return '; TOPS+0x%02X %s' % (a, VUMEM[a])
    m = re.search(r'([0-9]+)\(vi00\)', text)
    if m:
        return '; VU1 0x%03X (clipper state, outside the buffers)' % int(m.group(1))
    return ''


def disassemble(code, entries):
    n = len(code) // 8
    words = [struct.unpack_from('<II', code, i * 8) for i in range(n)]
    # first pass: branch targets
    labels = dict(entries)
    for pc, (lo, up) in enumerate(words):
        if up & 0x80000000:
            continue
        op = lo >> 25
        if op in (0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            t = pc + 1 + imm11(lo)
            labels.setdefault(t, 'L%03X' % t)
    lines = []
    for pc, (lo, up) in enumerate(words):
        if pc in labels:
            lines.append('')
            lines.append('%s:' % labels[pc])
        flags = ''
        if up & 0x80000000: flags += 'I'
        if up & 0x40000000: flags += 'E'
        if up & 0x20000000: flags += 'M'
        if up & 0x10000000: flags += 'D'
        if up & 0x08000000: flags += 'T'
        u = dis_upper(up & 0x07FFFFFF)
        if up & 0x80000000:
            fl = struct.unpack('<f', struct.pack('<I', lo))[0]
            l = 'LOI %.8g  (0x%08X)' % (fl, lo)
        else:
            l, _, _ = dis_lower(lo, pc, labels)
        note = annotate(l)
        lines.append(('  %03X  %-36s %-40s %-6s %s' % (pc, u, l, ('[' + flags + ']') if flags else '', note)).rstrip())
    return '\n'.join(lines) + '\n'


# The program entries the EE uses (vu1_lighting.c, vu1_prim.c, render.c,
# gs_driver.c): MSCAL addresses and ITOP values.
ENTRIES = {
    0x320: 'PRIM_320', 0x32A: 'PRIM_320_CULLA', 0x334: 'PRIM_320_CULLB',
    0x33E: 'PRIM_33E', 0x348: 'PRIM_33E_CULLA', 0x352: 'PRIM_33E_CULLB',
    0x35C: 'LIT_320', 0x366: 'LIT_320_CULLA', 0x370: 'LIT_320_CULLB',
    0x37A: 'LIT_33E', 0x384: 'LIT_33E_CULLA', 0x38E: 'LIT_33E_CULLB',
    0x04E: 'VU1L_DEFAULT', 0x057: 'VU1L_SPEC2_S6', 0x082: 'VU1L_SPEC2_S12', 0x0AF: 'VU1L_SPEC2',
    0x0DE: 'VU1L_SPEC4_S6', 0x115: 'VU1L_SPEC4_S12', 0x14C: 'VU1L_SPEC4', 0x185: 'VU1L_DIFFUSE',
    0x1AD: 'VU1L_LOCAL_S6', 0x1E1: 'VU1L_LOCAL_S12', 0x21A: 'VU1L_LOCAL', 0x257: 'VU1L_ENVMTX',
    0x297: 'VU1L_ENVMAP', 0x2DB: 'VU1L_CONST',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inc', default=DEFAULT_INC)
    ap.add_argument('--bin', help='write the unscrambled microcode here')
    ap.add_argument('-o', '--out', help='listing file (default stdout)')
    a = ap.parse_args()
    code = load_microcode(a.inc)
    if len(code) != 0x3DA8:
        print('warning: %d bytes (expected 0x3DA8)' % len(code), file=sys.stderr)
    if a.bin:
        open(a.bin, 'wb').write(code)
    entries = dict(ENTRIES)
    # the prologue entries (matrix -1, registers -5) and the format offsets
    for e, n in list(ENTRIES.items()):
        if n.startswith('PRIM') or n.startswith('LIT'):
            entries.setdefault(e - 1, n + '_M')
            entries.setdefault(e - 5, n + '_R')
            entries.setdefault(e - 6, n + '_RM')
        if n.startswith('VU1L'):
            for k in (2, 4, 6):
                entries.setdefault(e - k, '%s_F%d' % (n, k // 2))
    text = disassemble(code, entries)
    if a.out:
        open(a.out, 'w').write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
