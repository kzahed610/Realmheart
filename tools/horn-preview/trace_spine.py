#!/usr/bin/env python3
"""Piece A extractor v4: geodesic center-pull walk -- fragmentation-proof.

1. flood + breach cut + close -> clean horn mask (already proven good)
2. endpoints: BASE = mask point nearest breach anchor;
   TIP = mask point with max geodesic-ish distance (use farthest point by
   euclid distance among LOW-depth pixels, i.e. thin tail region)
3. walk: from base, repeatedly step to the unvisited 5x5-neighborhood pixel
   that maximizes (dist_transform value) - 0.02*(steps taken); this hugs
   the fattest corridor (the tube middle) and cannot leave the mask.
"""
import math
import numpy as np
from PIL import Image
from collections import deque
from scipy.ndimage import label as cc_label, binary_closing, distance_transform_edt

REF = "/tmp/hornprev/ref_full.png"
W_IMG, H_IMG = 1655, 931
ASPECT = W_IMG / H_IMG
N_KNOTS = 9
SAMPLES_PER_SEG = 3
BREACH_X, BREACH_Y = 679, 154

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

M[:, BREACH_X + 2:] = False
M = binary_closing(M, structure=np.ones((13, 13), bool))
lab, n = cc_label(M)
sizes = np.bincount(lab.ravel())
sizes[0] = 0
M = lab == sizes.argmax()

D = distance_transform_edt(M)
print("mask px:", M.sum(), " max halfwidth:", D.max())

# --- endpoints ---
ys, xs = np.where(M)
# base: nearest to breach anchor
db = np.hypot(ys - BREACH_Y, xs - BREACH_X)
bi = int(db.argmin())
base = (int(ys[bi]), int(xs[bi]))

# tip: thin pixels (D < 8) farthest (euclid) from the base
thin = np.where((M) & (D < 8))
dt = np.hypot(thin[0] - base[0], thin[1] - base[1])
ti = int(dt.argmax())
tip = (int(thin[0][ti]), int(thin[1][ti]))
print("base:", base, " tip:", tip)

# --- center-pull greedy walk on D ---
visited = np.zeros_like(M)
cur = base
path = [cur]
visited[cur] = True


def nbrs(p):
    out = []
    for da in (-1, 0, 1):
        for db_ in (-1, 0, 1):
            if da or db_:
                n = (p[0] + da, p[1] + db_)
                if 0 <= n[0] < Hh and 0 <= n[1] < Ww and M[n] and not visited[n]:
                    out.append(n)
    return out


guard = 0
while cur != tip and guard < 200000:
    guard += 1
    ns = nbrs(cur)
    if not ns:
        # dead end (shouldn't happen): allow revisits by resetting visited
        # in a small window around current point
        for da in range(-3, 4):
            for db_ in range(-3, 4):
                yy, xx = cur[0] + da, cur[1] + db_
                if 0 <= yy < Hh and 0 <= xx < Ww:
                    visited[yy, xx] = False
        visited[cur] = True
        for p_ in path[-40:]:
            visited[p_] = True
        ns = nbrs(cur)
        if not ns:
            break
    # score: depth (stay centered) minus progress toward tip penalty,
    # plus strong pull toward tip
    best, bscore = None, -1e18
    tdist = math.hypot(tip[0] - cur[0], tip[1] - cur[1])
    for n_ in ns:
        d_tip = math.hypot(tip[0] - n_[0], tip[1] - n_[1])
        score = D[n_] * 2.0 - d_tip * 1.0
        if score > bscore:
            best, bscore = n_, score
    cur = best
    path.append(cur)
    visited[cur] = True

print("walk nodes:", len(path))
P = np.array([[p[1], p[0]] for p in path], float)
HWv = np.array([D[p] for p in path], float)


def smooth(arr, w):
    k = np.ones(w) / w
    ext = np.concatenate([np.full(w, arr[0]), arr, np.full(w, arr[-1])])
    return np.convolve(ext, k, "valid")[: len(arr)]


P[:, 0] = smooth(P[:, 0], 13)
P[:, 1] = smooth(P[:, 1], 13)
HWv = smooth(HWv, 17)

seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
arc = np.concatenate([[0], np.cumsum(seg)])
total = arc[-1]
print(f"spine length {total:.0f}px  hw start {HWv[0]:.1f} end {HWv[-1]:.1f}")

knots = []
for i in range(N_KNOTS):
    uu = total * i / (N_KNOTS - 1)
    j = int(np.searchsorted(arc, uu))
    j = min(max(j, 1), len(P) - 1)
    t = (uu - arc[j - 1]) / max(arc[j] - arc[j - 1], 1e-6)
    knots.append((
        P[j - 1, 0] + (P[j, 0] - P[j - 1, 0]) * t,
        P[j - 1, 1] + (P[j, 1] - P[j - 1, 1]) * t,
        max(HWv[j - 1] + (HWv[j] - HWv[j - 1]) * t, 3.5),
    ))

print("\n# traced control knots (base -> tip):")
for k in knots:
    print(f"  img({k[0]:6.1f},{k[1]:6.1f}) hw={k[2]:5.1f}px")

# --- root design ---
# The reference occludes the true root behind crater debris (visible root
# tapers to ~1px -- an occlusion artifact, not the real thickness). Extend
# the curve up-right into the breach at armor grade, mirroring how the ref's
# fully-visible right horn enters the wall diagonally at near-full width.
ROOT_IMG = (668.0, 192.0)
ROOT_HW = 60.0
knots = [(ROOT_IMG[0], ROOT_IMG[1], ROOT_HW)] + knots[1:]
print("\n# FINAL knots (designed root + traced body):")
for k in knots:
    print(f"  img({k[0]:6.1f},{k[1]:6.1f}) hw={k[2]:5.1f}px")

# --- GLSL block ---
def catmull(p0, p1, p2, p3, t):
    o = []
    for i in (0, 1):
        t2, t3 = t * t, t * t * t
        o.append(0.5 * ((2 * p1[i]) + (-p0[i] + p2[i]) * t
                        + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t2
                        + (-p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]) * t3))
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
for a2, b2 in zip(samples, samples[1:]):
    kl.append(kl[-1] + math.hypot(b2[0] - a2[0], b2[1] - a2[1]))
tot = kl[-1]
kl = [kk / tot for kk in kl]


def width_at(uu):
    nn = len(Wk) - 1
    for i in range(nn):
        k0, k1 = i / nn, (i + 1) / nn
        if uu <= k1:
            tt = min(max((uu - k0) / max(k1 - k0, 1e-5), 0.0), 1.0)
            s = tt * tt * (3 - 2 * tt)
            return Wk[i] + (Wk[i + 1] - Wk[i]) * s
    return Wk[-1]


ws = [width_at(uu) for uu in kl]

out = [
    f"const int kSpineCount = {len(samples)};",
    f"const vec3 kSpine[{len(samples)}] = vec3[{len(samples)}](",
]
rr = []
for i, (x, yy) in enumerate(samples):
    z = 0.02 + 0.10 * math.sin(math.pi * min(kl[i] * 1.15, 1.0))
    rr.append(f"    vec3({x:.4f}, {yy:.4f}, {z:.3f})")
out.append(",\n".join(rr))
out.append(");")
out.append(f"const float kWidths[{len(ws)}] = float[{len(ws)}](\n    "
           + ", ".join(f"{w:.4f}" for w in ws) + "\n);")
out.append(f"const float kLens[{len(kl)}] = float[{len(kl)}](\n    "
           + ", ".join(f"{uu:.4f}" for uu in kl) + "\n);")

with open("/tmp/spine_block.txt", "w") as fh:
    fh.write("\n".join(out))
print(f"\nwrote /tmp/spine_block.txt ({len(samples)} samples)")

bx, by, _ = knots[0]
sx_b = (bx / W_IMG - 0.5) * ASPECT
sy_b = by / H_IMG - 0.5
print("BASE shader anchor: (%+.4f, %+.4f)  vs breach_l (-0.1180,-0.2720)"
      % (sx_b, sy_b))

# --- debug overlay ---
rgb = np.stack([img] * 3, -1)
rgb[M] = [0.55, 0.75, 1.0]
for p in path[::4]:
    rgb[p] = [1.0, 0.15, 0.15]
rgb[base] = [0.1, 1.0, 0.1]
rgb[tip] = [1.0, 1.0, 0.1]
Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(
    "/tmp/hornprev/skel_dbg.jpg", quality=88)
import base64
import pathlib
def b64(pth):
    return "data:image/jpeg;base64," + base64.b64encode(pathlib.Path(pth).read_bytes()).decode()
html = f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>Walk Debug</title>
<style>body{{background:#101014;color:#cdd6f4;font-family:sans-serif;margin:14px}}
img{{width:86vw;border-radius:6px;border:1px solid #313244}}</style></head><body>
<img src="{b64('/tmp/hornprev/skel_dbg.jpg')}"></body></html>"""
pathlib.Path("/tmp/hornprev/skel2.html").write_text(html)
print("debug written")
