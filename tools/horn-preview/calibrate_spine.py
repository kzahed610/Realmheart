#!/usr/bin/env python3
"""Closed-loop spine calibration.

Measures ref-vs-render edge positions along normals at every GLSL spine
sample, applies per-sample corrections, re-derives 9 Catmull knots, and
rewrites /tmp/spine_block.txt ready to splice.

Usage: python3 calibrate_spine.py <render.png>
"""
import math
import sys

import numpy as np
from PIL import Image
from collections import deque
from scipy.ndimage import binary_closing, label as cc_label, distance_transform_edt

REN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hornprev/v13_full.png"
REF = "/tmp/hornprev/ref_full.png"
BLOCK_IN = "/tmp/spine_block.txt"
W_IMG, H_IMG = 1655, 931
ASPECT = W_IMG / H_IMG
GAIN = 0.85          # correction gain per iteration
N_KNOTS = 9
SAMPLES_PER_SEG = 3


def build_ref_mask():
    img = np.asarray(Image.open(REF).convert("L")).astype(np.float32) / 255.0
    Hh, Ww = img.shape
    dark = img < 0.42
    seed = None
    for y in range(Hh // 6, Hh // 2):
        for x in range(0, Ww // 2):
            if dark[y, x]:
                seed = (y, x)
                break
        if seed:
            break
    M = np.zeros_like(dark)
    q = deque([seed])
    M[seed] = True
    while q:
        y, x = q.popleft()
        for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
            if 0 <= ny < Hh and 0 <= nx < Ww and not M[ny, nx] and dark[ny, nx]:
                M[ny, nx] = True
                q.append((ny, nx))
    M[:, 681:] = False
    Mraw = M.copy()
    Mc = binary_closing(M, structure=np.ones((13, 13), bool))
    lab, _ = cc_label(Mc)
    sizes = np.bincount(lab.ravel())
    sizes[0] = 0
    Mc = lab == sizes.argmax()
    return img, Mc, Mraw, distance_transform_edt(Mraw)


img, Mr, Mraw, Draw = build_ref_mask()
Hh, Ww = img.shape

ren = np.asarray(Image.open(REN).convert("L")).astype(np.float32) / 255.0
Mv = ren > 0.02
Mv[:, Ww // 2:] = False

# ---- parse current GLSL block ----
txt = open(BLOCK_IN).read()
import re

sp = re.findall(r"vec3\(\s*([-0-9.]+),\s*([-0-9.]+),", txt)
ws = re.findall(r"float\[25\]\(\s*(.*?)\)", txt, re.S)
wl = re.findall(r"kLens\[25\] = float\[25\]\(\s*(.*?)\n\)", txt, re.S)
assert len(sp) == 25, len(sp)
P = np.array([[(float(a) / ASPECT + 0.5) * Ww, (float(b) + 0.5) * Hh]
              for a, b in sp])
Wpx = np.array([float(v) for v in ws[0].replace(")", "").split(",")]) * H_IMG
U = np.array([float(v) for v in wl[0].replace(")", "").split(",")])

# ---- per-sample edge measurement ----
def ray(mask, cx, cy, dx, dy, maxlen):
    r = 1.0
    while r <= maxlen:
        xi, yi = int(round(cx + dx * r)), int(round(cy + dy * r))
        if not (0 <= xi < Ww and 0 <= yi < Hh) or not mask[yi, xi]:
            break
        r += 1.0
    return r - 1.0


tang = np.gradient(P, axis=0)
tl = np.linalg.norm(tang, axis=1)
tl[tl == 0] = 1
tang /= tl[:, None]
cx0, cy0 = P[:, 0].mean(), P[:, 1].mean()

newP = P.copy()
newW = Wpx.copy()
for i in range(len(P)):
    if i == 0:
        continue  # designed root
    tx, ty = tang[i]
    nx_, ny_ = -ty, tx
    if (P[i, 0] - cx0) * nx_ + (P[i, 1] - cy0) * ny_ < 0:
        nx_, ny_ = -nx_, -ny_
    cap = 150
    # ref edges (with fusion guard via pre-close medial disk)
    ro_r = ray(Mr, P[i, 0], P[i, 1], nx_, ny_, cap)
    ri_r = ray(Mr, P[i, 0], P[i, 1], -nx_, -ny_, cap)
    Dc = float(Draw[int(round(P[i, 1])), int(round(P[i, 0]))])
    ds = max(Dc, 8.0)
    if ri_r > 1.45 * ds and ri_r > 1.35 * max(ro_r, 8.0):
        ri_r = min(Dc, ro_r)
    elif ro_r > 1.45 * ds and ro_r > 1.35 * max(ri_r, 8.0):
        ro_r = min(Dc, ri_r)
    # render edges
    ro_v = ray(Mv, P[i, 0], P[i, 1], nx_, ny_, cap)
    ri_v = ray(Mv, P[i, 0], P[i, 1], -nx_, -ny_, cap)
    if ro_v + ri_v < 8:
        print(f"sample {i}: render degenerate, skip")
        continue
    d_c = ((ro_r - ri_r) - (ro_v - ri_v)) / 2.0
    d_w = ((ro_r + ri_r) - (ro_v + ri_v)) / 2.0
    newP[i, 0] += GAIN * d_c * nx_
    newP[i, 1] += GAIN * d_c * ny_
    newW[i] = max(Wpx[i] + GAIN * d_w, 4.0)
    print(f"s{i:02d} u={U[i]:.3f} dc={d_c:+6.1f} dw={d_w:+6.1f} -> hw {Wpx[i]:5.1f}->{newW[i]:5.1f}")

# ---- re-derive 9 knots from corrected samples ----
knot_idx = [round(j * (len(P) - 1) / (N_KNOTS - 1)) for j in range(N_KNOTS)]
knot_idx[0] = 0  # designed root preserved
knots = [(newP[k, 0], newP[k, 1], newW[k]) for k in knot_idx]
print("\nknots:", [(round(a), round(b), round(c)) for a, b, c in knots])


# ---- regenerate GLSL ----
def catmull(p0, p1, p2, p3, t):
    o = []
    for k in (0, 1):
        t2, t3 = t * t, t * t * t
        o.append(0.5 * ((2 * p1[k]) + (-p0[k] + p2[k]) * t
                        + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2
                        + (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3))
    return tuple(o)


Pk = [(k[0] / W_IMG - 0.5) * ASPECT for k in knots]
Py = [k[1] / H_IMG - 0.5 for k in knots]
Wk = [k[2] / H_IMG for k in knots]
pts = list(zip(Pk, Py))
ext = [pts[0]] + pts + [pts[-1]]
samples = []
for i in range(1, len(ext) - 2):
    p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
    for j in range(SAMPLES_PER_SEG):
        samples.append(catmull(p0, p1, p2, p3, j / SAMPLES_PER_SEG))
samples.append(pts[-1])
clean = [samples[0]]
for s in samples[1:]:
    if math.hypot(s[0] - clean[-1][0], s[1] - clean[-1][1]) > 1e-4:
        clean.append(s)
samples = clean
kl = [0.0]
for a, b in zip(samples, samples[1:]):
    kl.append(kl[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
tot = kl[-1]
kl = [u / tot for u in kl]


def width_at(uu):
    nn = len(Wk) - 1
    for i in range(nn):
        k0, k1 = i / nn, (i + 1) / nn
        if uu <= k1:
            tt = min(max((uu - k0) / max(k1 - k0, 1e-5), 0.0), 1.0)
            s = tt * tt * (3 - 2 * tt)
            return Wk[i] + (Wk[i + 1] - Wk[i]) * s
    return Wk[-1]


wsv = [width_at(u) for u in kl]
out = [
    f"const int kSpineCount = {len(samples)};",
    f"const vec3 kSpine[{len(samples)}] = vec3[{len(samples)}](",
]
rr = []
for i, (xx, yy) in enumerate(samples):
    z = 0.02 + 0.10 * math.sin(math.pi * min(kl[i] * 1.15, 1.0))
    rr.append(f"    vec3({xx:.4f}, {yy:.4f}, {z:.3f})")
out.append(",\n".join(rr))
out.append(");")
out.append(f"const float kWidths[{len(wsv)}] = float[{len(wsv)}](\n    "
           + ", ".join(f"{w:.4f}" for w in wsv) + "\n);")
out.append(f"const float kLens[{len(kl)}] = float[{len(kl)}](\n    "
           + ", ".join(f"{u:.4f}" for u in kl) + "\n);")
open("/tmp/spine_block.txt", "w").write("\n".join(out))
print(f"\nwrote calibrated /tmp/spine_block.txt ({len(samples)} samples)")
