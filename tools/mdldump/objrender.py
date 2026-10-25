#!/usr/bin/env python3
"""objrender.py - a tiny software renderer for mdldump's .obj files (numpy + pillow).

Draws the triangles back to front (painter's algorithm) with flat shading:
a key light from the camera's upper left plus ambient, both sides lit; with
--colors the vertex colours of the obj (mdldump writes RGBA / 128 for the
packets that carry colours) modulate it.  Enough to look at a decoded model.

  python objrender.py IN.obj OUT.png [--yaw DEG] [--pitch DEG] [--size W H]
                      [--top] [--colors] [--group SUBSTR] [--persp]

y is up (GT3's model space).  --top: orthographic view from above (x right,
z down the image).
"""
import argparse
import math
import sys

import numpy as np
from PIL import Image, ImageDraw


def load_obj(path, group=None):
    vs, cols, faces, fgroups, fmtl = [], [], [], [], []
    cur_group, cur_mtl = '', ''
    with open(path) as f:
        for line in f:
            if not line or line[0] == '#':
                continue
            if line.startswith('v '):
                p = line.split()
                vs.append((float(p[1]), float(p[2]), float(p[3])))
                cols.append((float(p[4]), float(p[5]), float(p[6])) if len(p) >= 7 else None)
            elif line.startswith('g '):
                cur_group = line[2:].strip()
            elif line.startswith('usemtl '):
                cur_mtl = line[7:].strip()
            elif line.startswith('f '):
                if group and group not in cur_group:
                    continue
                idx = [int(t.split('/')[0]) - 1 for t in line.split()[1:4]]
                faces.append(idx)
                fgroups.append(cur_group)
                fmtl.append(cur_mtl)
    return np.array(vs, dtype=np.float64), cols, np.array(faces, dtype=np.int64), fgroups, fmtl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('obj')
    ap.add_argument('png')
    ap.add_argument('--yaw', type=float, default=35.0)
    ap.add_argument('--pitch', type=float, default=20.0)
    ap.add_argument('--size', type=int, nargs=2, default=(1024, 768))
    ap.add_argument('--top', action='store_true')
    ap.add_argument('--colors', action='store_true')
    ap.add_argument('--group')
    ap.add_argument('--persp', action='store_true')
    ap.add_argument('--bg', default='40,44,52')
    a = ap.parse_args()

    v, cols, f, groups, mtls = load_obj(a.obj, a.group)
    if len(f) == 0:
        print('no faces')
        return 1
    used = np.unique(f.ravel())
    lo, hi = v[used].min(axis=0), v[used].max(axis=0)
    centre = (lo + hi) / 2
    radius = np.linalg.norm(hi - lo) / 2 or 1.0
    p = v - centre

    if a.top:
        # x right, z down: looking down -y
        cam = np.array([[1, 0, 0], [0, 0, -1], [0, -1, 0]], dtype=np.float64)  # rows: right, up, forward
        # screen x = x, screen y = -z ... keep z (north) up: up axis = -z
        cam = np.array([[1, 0, 0], [0, 0, -1], [0, -1, 0]], dtype=np.float64)
    else:
        yaw, pitch = math.radians(a.yaw), math.radians(a.pitch)
        # camera looks at the centre from (yaw, pitch); forward = into the screen
        fwd = np.array([-math.sin(yaw) * math.cos(pitch), -math.sin(pitch), -math.cos(yaw) * math.cos(pitch)])
        right = np.cross(fwd, [0, 1, 0])
        right /= np.linalg.norm(right)
        up = np.cross(right, fwd)
        cam = np.array([right, up, fwd])
    q = p @ cam.T           # (x right, y up, z depth into the screen)
    W, H = a.size
    scale = 0.95 * min(W, H) / (2 * radius)
    if a.persp and not a.top:
        dist = radius * 2.5
        zc = q[:, 2] + dist
        sx = W / 2 + q[:, 0] * scale * dist / np.maximum(zc, 1e-3)
        sy = H / 2 - q[:, 1] * scale * dist / np.maximum(zc, 1e-3)
    else:
        sx = W / 2 + q[:, 0] * scale
        sy = H / 2 - q[:, 1] * scale

    tri = q[f]                                  # (n, 3, 3)
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    nl = np.linalg.norm(n, axis=1)
    nl[nl == 0] = 1
    n /= nl[:, None]
    light = np.array([-0.4, 0.6, -0.7])
    light /= np.linalg.norm(light)
    shade = 0.35 + 0.65 * np.abs(n @ light)     # two-sided
    depth = tri[:, :, 2].mean(axis=1)
    order = np.argsort(-depth)                  # far first

    bg = tuple(int(c) for c in a.bg.split(','))
    img = Image.new('RGB', (W, H), bg)
    d = ImageDraw.Draw(img)
    base = np.array([200, 200, 205], dtype=np.float64)
    for i in order:
        a0, a1, a2 = f[i]
        if a.colors and cols[a0] is not None:
            c = np.array([cols[a0], cols[a1], cols[a2]]).mean(axis=0) * 255
        else:
            c = base
        c = np.clip(c * shade[i], 0, 255).astype(int)
        d.polygon([(sx[a0], sy[a0]), (sx[a1], sy[a1]), (sx[a2], sy[a2])], fill=tuple(c))
    img.save(a.png)
    print('%s: %d triangles, bbox %s .. %s' % (a.png, len(f), np.round(lo, 2), np.round(hi, 2)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
