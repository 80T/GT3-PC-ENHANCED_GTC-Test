"""capq.py - ad-hoc queries on a capture's draws.csv"""
import csv, sys, os
d = sys.argv[1]
rows = []
with open(os.path.join(d, "draws.csv"), newline="") as f:
    for r in csv.DictReader(f):
        for k in ("order", "parts", "count", "write_mask", "ztest", "zwrite", "zfunc", "date", "datm", "x0", "y0", "x1", "y1", "textured", "texture_is_target", "ps_inv", "run", "part", "mirror_pass", "stencil_ref"):
            r[k] = int(r[k])
        rows.append(r)
rows.sort(key=lambda r: r["order"])
def show(title, rs, n=60):
    print(f"\n== {title} ({len(rs)}) ==")
    print(f"{'order':>6} {'kind':<12} {'tgt':>6} {'prog':<8} {'form':<11} {'run':>4} {'part':>4} {'cnt':>5} {'bl':>4} {'mk':>2} {'z':>3} {'dt':>2} {'dm':>2} {'st':<12} {'mp':>2} {'tx':>2} {'PS Mpx':>7} area")
    for r in rs[:n]:
        print(f"{r['order']:>6} {r['kind']:<12} {r['target']:>6} {r['program']:<8} {r['ad_form']:<11} {r['run']:>4} {r['part']:>4} {r['count']:>5} {r['blend']:>4} {r['write_mask']:>2} {r['ztest']}{r['zwrite']}{r['zfunc']} {r['date']:>2} {r['datm']:>2} {r['stencil_op']:<12} {r['mirror_pass']:>2} {r['textured']}{'T' if r['texture_is_target'] else ' '} {int(r['ps_inv']) / 1e6:>7.3f} {r['x0']},{r['y0']}-{r['x1']},{r['y1']}")
q = sys.argv[2]
if q == "early":
    show("the first 60 calls", rows, 60)
    show("the first 40 DATE draws", [r for r in rows if r["date"] == 1], 40)
    show("the first draws with a stencil op", [r for r in rows if r["stencil_op"] != "-"], 12)
elif q == "flare":
    small = [r for r in rows if r["kind"] == "gs batch" and r["write_mask"] == 8 and r["textured"] == 0 and (r["x1"] - r["x0"]) < 2000]
    show("alpha-only untextured GS sprites (not full screen)", small, 12)
    fl = [r for r in rows if r["program"] == "flare"]
    show("flare program draws", fl, 8)
    print("flare draws: date", sum(r["date"] for r in fl), "ad forms", set(r["ad_form"] for r in fl), "blends", set(r["blend"] for r in fl), "targets", set(r["target"] for r in fl))
    after = [r for r in rows if r["order"] > small[0]["order"]] if small else []
    print("after the first alpha sprite: DATE draws", sum(r["date"] for r in after), "C = Ad forms", sum(1 for r in after if r["ad_form"] != "-"), "shader forms", sum(1 for r in after if r["ad_form"] == "shader"))
elif q == "end":
    show("the last 50 calls", rows[-50:], 50)
elif q == "range":
    a, b = int(sys.argv[3]), int(sys.argv[4])
    show(f"orders {a}..{b}", [r for r in rows if a <= r["order"] <= b], 200)
elif q == "date":
    from collections import OrderedDict
    for name, sel in (("DATE draws", lambda r: r["date"] == 1), ("mirror passes", lambda r: r["kind"] == "mirror pass"),
                      ("draws WITH a mirror pass", lambda r: r["mirror_pass"] == 1)):
        g = OrderedDict()
        for r in rows:
            if sel(r):
                a = g.setdefault(r["target"], [0, 0.0, r["order"], r["order"], set()])
                a[0] += 1; a[1] += r["ps_inv"] / 1e6; a[3] = r["order"]; a[4].add(r["program"])
        print(f"\n== {name} by target ==")
        for t, a in g.items():
            print(f"  {t:>6}: n {a[0]:>5} PS {a[1]:>7.2f} Mpx orders {a[2]}..{a[3]} programs {sorted(a[4])}")
    # mirror pass fill along the frame
    mir = [r for r in rows if r["kind"] == "mirror pass"]
    print("\n== mirror pass Mpx by order decile ==")
    n = len(rows)
    for b in range(10):
        lo, hi = rows[b * n // 10]["order"], rows[(b + 1) * n // 10 - 1]["order"]
        seg = [r for r in mir if lo <= r["order"] <= hi]
        print(f"  orders {lo:>5}-{hi:>5}: {len(seg):>4} passes {sum(r['ps_inv'] for r in seg) / 1e6:>7.2f} Mpx")
    # what the DATE draws into the frame target are
    frame_t = max(set(r["target"] for r in rows), key=lambda t: sum(r["ps_inv"] for r in rows if r["target"] == t and r["kind"] != "gs batch"))
    show(f"DATE draws into {frame_t}", [r for r in rows if r["date"] == 1 and r["target"] == frame_t], 30)
elif q == "big":
    big = [r for r in rows if r["kind"] in ("gs batch", "copy", "resolve", "clear") and int(r["ps_inv"]) >= 400000]
    print(f"\n== GS batches / copies / clears >= 0.4 Mpx ({len(big)}) ==")
    for r in big:
        print(f"{r['order']:>6} {r['kind']:<9} {r['target']:>6} {r['target_w']}x{r['target_h']:<5} cnt {r['count']:>4} bl {r['blend']} mk {r['write_mask']:>2} z {r['ztest']}{r['zwrite']}{r['zfunc']} st {r['stencil_op']:<8} tex {r['textured']}{'T' if r['texture_is_target'] else ' '} "
              f"PS {int(r['ps_inv']) / 1e6:>6.2f} area {r['x0']},{r['y0']}-{r['x1']},{r['y1']} binds {r['bind_count']} thr {r['thread']} note {r['note']!r} prim {r['prim']}")
    # the mirror passes >= 0.2 Mpx with the draw before
    byo = {r["order"]: r for r in rows}
    print("\n== mirror passes >= 0.2 Mpx ==")
    for r in rows:
        if r["kind"] == "mirror pass" and int(r["ps_inv"]) >= 200000:
            p = byo.get(r["order"] - 1)
            print(f"{r['order']:>6} PS {int(r['ps_inv']) / 1e6:>6.2f} parts {r['parts']} <- {p['kind']} {p['program']} run {p['run']} part {p['part']} mk {p['write_mask']} bl {p['blend']} tex {p['textured']} area {p['x0']},{p['y0']}-{p['x1']},{p['y1']} PS {int(p['ps_inv']) / 1e6:.2f}")
elif q == "gs":
    from collections import OrderedDict
    gs = [r for r in rows if r["kind"] == "gs batch"]
    g = OrderedDict()
    for r in gs:
        w, h = r["x1"] - r["x0"], r["y1"] - r["y0"]
        full = "full" if w >= int(r["target_w"]) - 2 and h >= int(r["target_h"]) - 2 else "part"
        k = (r["target"], f"{r['target_w']}x{r['target_h']}", full, r["blend"], r["write_mask"], f"{r['textured']}{'T' if r['texture_is_target'] else ''}", r["stencil_op"], r["date"])
        a = g.setdefault(k, [0, 0.0, 0])
        a[0] += 1; a[1] += r["ps_inv"] / 1e6; a[2] += r["count"]
    print(f"\n== GS batches: target size full/part blend mask tex stencil date : n, PS Mpx, verts ==")
    for k, a in sorted(g.items(), key=lambda kv: -kv[1][1]):
        print(f"  {k[0]:>6} {k[1]:>9} {k[2]} bl {k[3]} mk {k[4]:>2} tex {k[5]:<2} st {k[6]:<8} dt {k[7]}: n {a[0]:>4} PS {a[1]:>7.2f} Mpx verts {a[2]:>6}")
    print("total GS", sum(r["ps_inv"] for r in gs) / 1e6, "Mpx; frame total", sum(r["ps_inv"] for r in rows) / 1e6)
