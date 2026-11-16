"""capdis.py - dissect a frame capture's draws.csv (pgl_capture.h).
usage: python -I -X utf8 capdis.py <capture dir> [--top N]"""
import csv, sys, os
from collections import defaultdict, OrderedDict

d = sys.argv[1]
top = 30
if "--top" in sys.argv:
    top = int(sys.argv[sys.argv.index("--top") + 1])
rows = []
with open(os.path.join(d, "draws.csv"), newline="") as f:
    for r in csv.DictReader(f):
        for k in ("order", "id", "thread", "run", "part", "parts", "instances", "count", "write_mask", "ztest",
                  "zwrite", "zfunc", "date", "datm", "stencil_ref", "mirror_pass", "fold", "flat", "slots",
                  "bind_count", "cpu_ns", "target_w", "target_h", "x0", "y0", "x1", "y1", "depth_frame", "textured",
                  "texture_is_target", "ia_verts", "ia_prims", "vs_inv", "rast_prims", "ps_inv"):
            r[k] = int(r[k])
        r["gpu_us"] = float(r["gpu_us"])
        r["target"] = int(r["target"], 16)
        rows.append(r)
rows.sort(key=lambda r: r["order"])
M = 1e6
tot_ps = sum(r["ps_inv"] for r in rows)
print(f"{len(rows)} calls, PS {tot_ps / M:.1f} Mpx")

def table(title, key, rs, limit=40):
    agg = OrderedDict()
    for r in rs:
        k = key(r)
        a = agg.setdefault(k, [0, 0, 0.0, 0.0, 0, 0])
        a[0] += 1
        a[1] += r["parts"]
        a[2] += r["ps_inv"] / M
        a[3] += r["gpu_us"]
        a[4] += r["ia_verts"]
        a[5] += r["bind_count"]
    items = sorted(agg.items(), key=lambda kv: -kv[1][2])
    print(f"\n== {title} ==")
    print(f"{'key':<64} {'n':>5} {'parts':>6} {'PS Mpx':>8} {'%fill':>6} {'GPU us':>8} {'verts k':>8} {'binds':>6}")
    for k, a in items[:limit]:
        print(f"{str(k):<64} {a[0]:>5} {a[1]:>6} {a[2]:>8.2f} {100 * a[2] * M / max(1, tot_ps):>6.1f} {a[3]:>8.0f} {a[4] / 1e3:>8.1f} {a[5] / max(1, a[0]):>6.2f}")

gs = [r for r in rows if r["kind"] == "gs batch"]
table("GS batches by target / prim / blend / mask / textured(target?) / date", lambda r: (
    f"0x{r['target']:X} {r['target_w']}x{r['target_h']} {r['prim']} blend {r['blend']} mask {r['write_mask']} "
    f"tex {r['textured']}{'T' if r['texture_is_target'] else ''} date {r['date']} ad {r['ad_form']}"), gs)
print("\n== the 30 GS batches with the most fill ==")
print(f"{'order':>6} {'target':>8} {'native':>10} {'verts':>6} {'PS Mpx':>7} {'area':>22} {'blend':>5} {'mask':>4} {'tex':>3} {'date':>4} {'z':>3} {'thread':>6} {'cpu us':>7}")
for r in sorted(gs, key=lambda r: -r["ps_inv"])[:30]:
    print(f"{r['order']:>6} 0x{r['target']:>6X} {r['target_w']}x{r['target_h']:<5} {r['count']:>6} {r['ps_inv'] / M:>7.3f} "
          f"{r['x0']:>5},{r['y0']:>4}-{r['x1']:>5},{r['y1']:>4} {r['blend']:>5} {r['write_mask']:>4} "
          f"{r['textured']}{'T' if r['texture_is_target'] else ' '} {r['date']:>4} {r['ztest']}{r['zwrite']} {r['thread']:>6} {r['cpu_ns'] / 1000:>7.1f}")

mir = [r for r in rows if r["kind"] == "mirror pass"]
byorder = {r["order"]: r for r in rows}
def prev_kind(r):
    p = byorder.get(r["order"] - 1)
    if p is None:
        return "?"
    return f"{p['kind']}/{p['program']}/{p['ad_form']}"
table("mirror passes by the draw before them (kind/program/form) and target", lambda r: (prev_kind(r), f"0x{r['target']:X}"), mir)
print("\n== the 20 mirror passes with the most fill (and the draw before each) ==")
for r in sorted(mir, key=lambda r: -r["ps_inv"])[:20]:
    p = byorder.get(r["order"] - 1)
    print(f"{r['order']:>6} 0x{r['target']:X} parts {r['parts']:>3} PS {r['ps_inv'] / M:>6.3f} Mpx  before: {p['kind'] if p else '?'} "
          f"{p['program'] if p else ''} run {p['run'] if p else ''} part {p['part'] if p else ''} tex {p['textured'] if p else ''} "
          f"{'T' if p and p['texture_is_target'] else ''} blend {p['blend'] if p else ''} PS {(p['ps_inv'] / M) if p else 0:.3f}")

vu1 = [r for r in rows if r["kind"] in ("vu1", "vu1 merged")]
table("VU1 draws by program / form / target", lambda r: (r["program"], r["ad_form"], f"0x{r['target']:X}"), vu1)
table("VU1 draws by merge reason", lambda r: r["merge_why"], vu1)
table("VU1 'none' merge reason by the kind/program/form of the draw before", lambda r: prev_kind(r),
      [r for r in vu1 if r["merge_why"] == "none"], 15)
table("all calls by thread", lambda r: r["thread"], rows)
table("all calls by bind count", lambda r: r["bind_count"], rows, 20)

# the frame's phases: fill in order, bucketed
print("\n== fill along the frame (cumulative PS Mpx by order, 20 buckets) ==")
n = len(rows)
for b in range(20):
    seg = rows[b * n // 20:(b + 1) * n // 20]
    ps = sum(r["ps_inv"] for r in seg) / M
    kinds = defaultdict(float)
    for r in seg:
        kinds[r["kind"] if r["kind"] != "vu1 merged" else "vu1"] += r["ps_inv"] / M
    tk = ", ".join(f"{k} {v:.1f}" for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:3])
    tg = defaultdict(int)
    for r in seg:
        tg[r["target"]] += 1
    tt = ",".join(f"0x{t:X}" for t, c in sorted(tg.items(), key=lambda kv: -kv[1])[:2])
    print(f"orders {seg[0]['order']:>5}-{seg[-1]['order']:>5}: PS {ps:>6.1f} Mpx  ({tk})  targets {tt}")

# env passes vs bodies per run
print("\n== per run: parts, draws, fill by form ==")
runs = defaultdict(lambda: defaultdict(float))
for r in vu1:
    if r["run"] >= 0:
        runs[r["run"]]["draws"] += 1
        runs[r["run"]]["parts"] += r["parts"]
        runs[r["run"]][f"ps_{r['ad_form']}"] += r["ps_inv"] / M
        runs[r["run"]]["ps"] += r["ps_inv"] / M
        runs[r["run"]]["gpu"] += r["gpu_us"]
for seq in sorted(runs, key=lambda s: -runs[s]["ps"])[:top]:
    a = runs[seq]
    env = sum(v for k, v in a.items() if k.startswith("ps_") and k != "ps_-")
    print(f"run {seq:>3}: draws {int(a['draws']):>4} parts {int(a['parts']):>4} PS {a['ps']:>6.2f} Mpx (env forms {env:.2f}) GPU us {a['gpu']:>7.0f}")
