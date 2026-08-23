#!/usr/bin/env python3
"""Broken Seal horn spine generator.

Regenerates the kSpine/kWidths/kLens GLSL block in
effects/lockscreen/crystal/crystal.frag from a handful of control knots.

Usage: edit P (control knots, y-down space, z auto-arched) and W (widths at
knots), then run:  python3 spine_gen.py   -> writes /tmp/spine_block.txt
Then replace the block in crystal.frag between "const int kSpineCount" and
the end of the kLens array (wholesale replacement — never regex-patch rows).

Rules learned the hard way (2026-08-22 session):
- Base offset from center must EXCEED base width or the two horns merge
  into an M shape.
- Silhouette stays SMOOTH: plates are shading-only in the shader; the spine
  must never bulge per-segment.
- Tail knots: last 2-3 knots define the tip flick direction (outward!).
- Dedupe samples closer than 1e-4 (degenerate segments break the SDF).
"""
import math

# Control knots (x, y) in y-down normalized space. Left horn; mirrored +x.
P = [
    (-0.118, -0.272),  # base at the breach
    (-0.240, -0.320),  # sweep out-left
    (-0.360, -0.232),
    (-0.409, -0.085),  # far left of the curl
    (-0.398,  0.065),
    (-0.314,  0.175),
    (-0.282,  0.212),  # bottom of the curl
    (-0.292,  0.172),  # turn
    (-0.332,  0.122),  # tip: flicks OUTWARD (up-left)
]
# Tube width at each control knot.
W = [0.088, 0.092, 0.0916, 0.0846, 0.072, 0.054, 0.036, 0.022, 0.010]

SAMPLES_PER_SEG = 3


def catmull(p0, p1, p2, p3, t):
    out = []
    for i in (0, 1):
        t2, t3 = t * t, t * t * t
        v = 0.5 * (
            (2 * p1[i])
            + (-p0[i] + p2[i]) * t
            + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t2
            + (-p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]) * t3
        )
        out.append(v)
    return tuple(out)


def main():
    ext = [P[0]] + list(P) + [P[-1]]
    samples = []
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for j in range(SAMPLES_PER_SEG):
            samples.append(catmull(p0, p1, p2, p3, j / SAMPLES_PER_SEG))
    samples.append(P[-1])

    clean = [samples[0]]
    for s in samples[1:]:
        if math.hypot(s[0] - clean[-1][0], s[1] - clean[-1][1]) > 1e-4:
            clean.append(s)
    samples = clean

    kl = [0.0]
    for a, b in zip(samples, samples[1:]):
        kl.append(kl[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    total = kl[-1]
    kl = [k / total for k in kl]

    def width_at(u):
        n = len(W) - 1
        for i in range(n):
            k0, k1 = i / n, (i + 1) / n
            if u <= k1:
                t = min(max((u - k0) / max(k1 - k0, 1e-5), 0.0), 1.0)
                s = t * t * (3 - 2 * t)
                return W[i] + (W[i + 1] - W[i]) * s
        return W[-1]

    ws = [width_at(u) for u in kl]
    f = lambda v: f"{v:.4f}"

    out = [
        f"const int kSpineCount = {len(samples)};",
        f"const vec3 kSpine[{len(samples)}] = vec3[{len(samples)}](",
    ]
    rows = []
    for i, (x, y) in enumerate(samples):
        z = 0.02 + 0.10 * math.sin(math.pi * min(kl[i] * 1.15, 1.0))
        rows.append(f"    vec3({f(x)}, {f(y)}, {z:.3f})")
    out.append(",\n".join(rows))
    out.append(");")
    out.append(
        f"const float kWidths[{len(ws)}] = float[{len(ws)}](\n    "
        + ", ".join(f(w) for w in ws)
        + "\n);"
    )
    out.append(
        f"const float kLens[{len(kl)}] = float[{len(kl)}](\n    "
        + ", ".join(f(u) for u in kl)
        + "\n);"
    )
    with open("/tmp/spine_block.txt", "w") as fh:
        fh.write("\n".join(out))
    print(f"wrote /tmp/spine_block.txt ({len(samples)} samples)")
    print("breach points should match the first knot:", P[0])


if __name__ == "__main__":
    main()
