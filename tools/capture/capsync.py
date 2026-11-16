"""capsync.py - the copies, passes and clears of a capture, in order"""
import csv, sys
for d in sys.argv[1:]:
    rows = sorted(csv.DictReader(open(d + r"\draws.csv", newline="")), key=lambda r: int(r["order"]))
    print("capture", d)
    for r in rows:
        if r["kind"] in ("pass", "copy", "resolve", "clear"):
            print(f"  {int(r['order']):>5} {r['kind']:<8} {r['target']:>6} {r['target_w']}x{r['target_h']} cnt {r['count']:>8} PS {int(r['ps_inv']) / 1e6:6.2f} note {r['note']!r}")
