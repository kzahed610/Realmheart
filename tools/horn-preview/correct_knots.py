#!/usr/bin/env python3
"""Piece A auto-corrector: fit knots to the ref mask via normal rays.

Reads current FINAL knots (designed root + traced body), computes local
tangent/normal per knot, ray-marches the REF horn mask along +/-normal,
re-fits center (midpoint) + halfwidth (half-span). Emits corrected GLSL.
"""
import math
import numpy as np
from PIL import Image
from collections import deque
from scipy.ndimage import binary_closing, label as cc_label

REF = "/tmp/hornprev/ref_full.png"
W_IMG, H_IMG = 1655, 931
ASPECT = W_IMG / H_IMG
N_KNOTS = 9
SAMPLES_PER_SEG = 3

# FINAL knots from trace_spine.py (img px): x, y, halfwidth
KNOTS = [
    (668.0, 192.0, 60.0),
    (614.5, 237.9, 64.1),
    (530.5, 237.3, 75.8),
    (459.5, 280.6, 68.0),
    (443.8, 362.9, 56.1),
    (461.0, 447.5, 47.6),
    (429.0, 524.8, 32.5),
    (350.1, 556.3, 16.6),
    (264.0, 550.2, 3.5),
]

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
Mraw = M.copy()                       # pre-close truth (seams still open)
M = binary_closing(M, structure=np.ones((13, 13), bool))
lab, n = cc_label(M)
sizes = np.bincount(lab.ravel())
sizes[0] = 0
M = lab == sizes.argmax()
from scipy.ndimage import distance_transform_edt
Draw = distance_transform_edt(Mraw)   # ~0 exactly at seam bridges
print("ref horn px:", M.sum())


def ray_extent(cx, cy, dx, dy, maxlen=140):
    """Distance from (cx,cy) along (dx,dy) until leaving the mask.

    A ray crossing a bright plate-seam bridge (sealed by close()) shows a
    Draw collapse to <2px -- treat as leaving the tube.
    """
    last_in = 0.0
    r = 1.0
    while r <= maxlen:
        xi, yi = int(round(cx + dx * r)), int(round(cy + dy * r))
        if not (0 <= xi < Ww and 0 <= yi < Hh) or not M[yi, xi]:
            break
        if Draw[yi, xi] < 2.0:
            break
        last_in = r
        r += 1.0
    return last_in


corrected = list(KNOTS)
for _pass in range(2):
    next_knots = []
    for i, (x, y, w) in enumerate(corrected):
        if i == 0:
            # Root is DESIGNED (ref debris occludes the true root -- rays here
            # measure debris edges, not horn edges). Occluded = design territory.
            next_knots.append((x, y, w))
            print(f"[p{_pass}] knot {i}: DESIGNED root, kept ({x:.0f},{y:.0f}) hw={w:.1f}")
            continue
        # tangent from neighbors
        ax, ay = corrected[max(i - 1, 0)][0], corrected[max(i - 1, 0)][1]
        bx_, by = corrected[min(i + 1, len(corrected) - 1)][0], corrected[min(i + 1, len(corrected) - 1)][1]
        tx, ty = bx_ - ax, by - ay
        tl = math.hypot(tx, ty) or 1.0
        tx, ty = tx / tl, ty / tl
        nx_, ny_ = -ty, tx  # normal
        # orient normal outward = away from the curl centroid
        cx0 = sum(k[0] for k in corrected) / len(corrected)
        cy0 = sum(k[1] for k in corrected) / len(corrected)
        if (x - cx0) * nx_ + (y - cy0) * ny_ < 0:
            nx_, ny_ = -nx_, -ny_

        # Rays measure LOCAL tube extent only -- cap so they cannot slip
        # across the C-pinch seam into the opposite limb of the curl.
        cap = 2.0 * w + 28.0
        r_out = ray_extent(x, y, nx_, ny_, maxlen=int(cap))
        r_in = ray_extent(x, y, -nx_, -ny_, maxlen=int(cap))
        if r_in > 1.5 * max(r_out, 8.0) and r_in > 1.4 * w:
            # Inward ray ran through a FUSED fold (upper-limb inner edge
            # merges into the descent limb at the C-pinch -- no seam in the
            # mask to stop it). Trust the traced width on the inner side.
            print(f"[p{_pass}] knot {i}: inward fusion (in={r_in:.0f} out={r_out:.0f} w={w:.0f}), clamping")
            r_in = w
        # Medial-disk sanity (Blum, pointwise): an honest ray through tube
        # interior ends at ~Draw(center); a ray crossing a LIMB-FUSION runs
        # multiples past it AND wildly asymmetric vs the opposite ray
        # (fusion = one-sided blow-through). Symmetric spans > D are just
        # high-curvature elbows (inscribed disk < chord span) -- keep them.
        Dc = float(Draw[int(round(y)), int(round(x))])
        dscale = max(Dc, 8.0)
        if r_in > 1.45 * dscale and r_in > 1.35 * r_out:
            print(f"[p{_pass}] knot {i}: inward fusion (in={r_in:.0f} out={r_out:.0f} D={Dc:.0f}), clamping")
            r_in = min(Dc, r_out)
        elif r_out > 1.45 * dscale and r_out > 1.35 * r_in:
            print(f"[p{_pass}] knot {i}: outward fusion (out={r_out:.0f} in={r_in:.0f} D={Dc:.0f}), clamping")
            r_out = min(Dc, r_in)
        span = r_out + r_in
        if span < 6:
            # degenerate ray (root may sit off-body); keep old width
            next_knots.append((x, y, w))
            print(f"[p{_pass}] knot {i}: rays degenerate, kept ({x:.0f},{y:.0f}) hw={w:.1f}")
            continue
        new_w = span / 2.0
        shift = (r_out - r_in) / 2.0
        new_x = x + nx_ * shift
        new_y = y + ny_ * shift
        # blend 70% toward measured (stability)
        fx = x + 0.7 * (new_x - x)
        fy = y + 0.7 * (new_y - y)
        fw = w + 0.7 * (new_w - w)
        fw = max(fw, 3.5)
        next_knots.append((fx, fy, fw))
        print(f"[p{_pass}] knot {i}: ({x:6.1f},{y:6.1f}) hw={w:5.1f} -> ({fx:6.1f},{fy:6.1f}) hw={fw:5.1f}  [out={r_out:.0f} in={r_in:.0f}]")
    corrected = next_knots

with open("/tmp/knots_corrected.txt", "w") as fh:
    fh.write(repr(corrected))

# --- regenerate GLSL ---
def catmull(p0, p1, p2, p3, t):
    o = []
    for i in (0, 1):
        t2, t3 = t * t, t * t * t
        o.append(0.5 * ((2 * p1[i]) + (-p0[i] + p2[i]) * t
                        + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t2
                        + (-p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]) * t3))
    return tuple(o)


Pk = [(k[0] / W_IMG - 0.5) * ASPECT for k in corrected]
Py = [k[1] / H_IMG - 0.5 for k in corrected]
Wk = [k[2] / H_IMG for k in corrected]
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
for i, (xx, yy) in enumerate(samples):
    z = 0.02 + 0.10 * math.sin(math.pi * min(kl[i] * 1.15, 1.0))
    rr.append(f"    vec3({xx:.4f}, {yy:.4f}, {z:.3f})")
out.append(",\n".join(rr))
out.append(");")
out.append(f"const float kWidths[{len(ws)}] = float[{len(ws)}](\n    "
           + ", ".join(f"{w:.4f}" for w in ws) + "\n);")
out.append(f"const float kLens[{len(kl)}] = float[{len(kl)}](\n    "
           + ", ".join(f"{uu:.4f}" for uu in kl) + "\n);")
with open("/tmp/spine_block.txt", "w") as fh:
    fh.write("\n".join(out))
print(f"\nwrote corrected /tmp/spine_block.txt ({len(samples)} samples)")
