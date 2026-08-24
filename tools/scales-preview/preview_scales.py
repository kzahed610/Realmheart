#!/usr/bin/env python3
"""Offline previewer for effects/lockscreen/scales/scales.frag.

Faithful numpy port of the shader math (same hash/noise/fbm, same palette,
same premultiplied-alpha compositing) rendered over a dark stand-in wallpaper.
Lets aesthetic iteration happen without contaminating the live desktop.

Usage:
    python3 tools/scales-preview/preview_scales.py [--out DIR] [--width W] [--height H]
Renders a labeled contact sheet of the four scene states to OUT.
"""
from __future__ import annotations

import argparse
import os

import numpy as np
from PIL import Image, ImageDraw

# ---- palette (must mirror ScalesRenderer.cpp kBg/kLine/kGlow/kWarnC) -------
BG = np.array([0.090, 0.078, 0.110])    # #17141c deep void
LINE = np.array([0.353, 0.333, 0.388])  # #5a5563 lavender-grey rims
GLOW = np.array([0.910, 0.757, 0.353])  # #E8C15A gold accents
WARN_C = np.array([0.702, 0.149, 0.118])  # #b3261e wrong-password red

CELL_FRAC = 0.058   # scale size in screen-height fractions (shader constant)
TARGET = 0.34       # final blob radius (screen-height fractions)


def fract(x: np.ndarray) -> np.ndarray:
    return x - np.floor(x)


def hash12(p: np.ndarray) -> np.ndarray:
    """Vectorized port of the shader's hash12."""
    p3 = np.stack([p[..., 0], p[..., 1], p[..., 0]], axis=-1) * 0.1031
    p3 = fract(p3)
    dot = (p3[..., 0] * (p3[..., 1] + 33.33)
           + p3[..., 1] * (p3[..., 2] + 33.33)
           + p3[..., 2] * (p3[..., 0] + 33.33))
    p3 = p3 + dot[..., None]
    return fract((p3[..., 0] + p3[..., 1]) * p3[..., 2])


def smoothstep(e0: float, e1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def noise2(p: np.ndarray) -> np.ndarray:
    i = np.floor(p)
    f = p - i
    u = f * f * (3.0 - 2.0 * f)
    h00 = hash12(i)
    h10 = hash12(i + [1.0, 0.0])
    h01 = hash12(i + [0.0, 1.0])
    h11 = hash12(i + [1.0, 1.0])
    return ((h00 * (1 - u[..., 0]) + h10 * u[..., 0]) * (1 - u[..., 1])
            + (h01 * (1 - u[..., 0]) + h11 * u[..., 0]) * u[..., 1])


def fbm(p: np.ndarray) -> np.ndarray:
    v = np.zeros(p.shape[:-1])
    a = 0.55
    q = p.copy()
    for k in range(3):
        v += noise2(q) * a
        q = q * 2.13 + 17.0
        a *= 0.5
    return v


def ease_out_cubic(t):
    f = 1.0 - t
    return 1.0 - f * f * f


# ---- one scale face (mirrors scale_face() in scales.frag) ------------------
K_CENTER = np.array([0.5, 0.10])
K_RADIUS = 0.97
K_SQUASH = 1.18


def scale_face(fy: np.ndarray, fx: np.ndarray):
    """fy, fx: cell-local coords in [0,1]. Returns rim, body, crease masks."""
    qx = (fx - K_CENTER[0]) * K_SQUASH
    qy = fy - K_CENTER[1]
    d = np.hypot(qx, qy)
    rim = (smoothstep(K_RADIUS - 0.055, K_RADIUS - 0.010, d)
           * smoothstep(K_RADIUS + 0.008, K_RADIUS - 0.012, d))
    inside = smoothstep(K_RADIUS + 0.004, K_RADIUS - 0.030, d)
    body = np.clip(d / K_RADIUS, 0.0, 1.0) ** 1.15 * inside
    crease = (smoothstep(K_RADIUS - 0.004, K_RADIUS + 0.075, d)
              * (1.0 - smoothstep(K_RADIUS + 0.075, K_RADIUS + 0.150, d)))
    return rim, body, crease


def render_frame(width: int, height: int, *, time_s: float, reveal: float,
                 opening: bool, warn: float, seed: float,
                 bg_image: np.ndarray) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width]
    uv_y = 1.0 - yy / height          # GL convention: v=0 at bottom
    uv_x = xx / width
    aspect = np.array([width / max(height, 1), 1.0])
    c = np.stack([(uv_x - 0.5) * aspect[0], uv_y - 0.5], axis=-1)
    length_c = np.linalg.norm(c, axis=-1)

    # torn central blob
    radius = 0.05 + (TARGET - 0.05) * ease_out_cubic(reveal)
    tendril = fbm(c * 3.0 + seed) - 0.5
    blob_edge = radius * (1.0 + tendril * 0.35)

    # imbricate grid (rows run across, alternate rows offset half a scale)
    sc = CELL_FRAC * height
    g_x = uv_x * width / sc
    g_y = uv_y * height / sc
    row = np.floor(g_y)
    stagger = 0.5 * np.mod(row, 2.0)
    gx = g_x + stagger
    id_x = np.floor(gx)
    id_y = row
    f_x = gx - id_x
    f_y = g_y - id_y

    h1 = hash12(np.stack([id_x + seed, id_y], axis=-1))
    h2 = hash12(np.stack([id_x + 7.7 + seed, id_y], axis=-1))
    h3 = hash12(np.stack([id_x + 31.3 + seed, id_y], axis=-1))
    phase = h1 * 6.2831
    jitter = h2 * 0.25

    rim, body, crease = scale_face(f_y, f_x)
    fx_up = (g_x + 0.5) - np.floor(g_x + 0.5)
    fy_up = (g_y + 1.0) - np.floor(g_y + 1.0)
    rim_u, body_u, crease_u = scale_face(fy_up, fx_up)
    crease_i = np.maximum(crease, crease_u)

    px_x = (id_x + 0.5 - stagger) * sc
    px_y = (id_y + 0.5) * sc
    cc_x = px_x / width - 0.5
    cc_y = 1.0 - px_y / height - 0.5
    cc = np.stack([cc_x * aspect[0], cc_y * aspect[1]], axis=-1)
    cell_dist = np.linalg.norm(cc, axis=-1)

    if opening:
        life = smoothstep(0.12, 0.88, reveal * 2.0 - cell_dist - jitter)
    else:
        erosion = 1.0 - reveal
        dissolve = smoothstep(cell_dist + jitter - 0.07,
                              cell_dist + jitter + 0.07, erosion)
        life = 1.0 - dissolve

    region = 1.0 - smoothstep(blob_edge, blob_edge + 0.05, cell_dist)

    mottle = 0.55 + 0.90 * h2
    shimmer = 0.88 + 0.12 * np.sin(time_s * 0.8 + phase)
    breath = 0.70 + 0.30 * np.sin(time_s * 1.5 + phase * 3.0)

    a_mem = 0.16 * region * life
    a_body = body * 0.42 * mottle * shimmer * region * life
    a_crease = crease_i * 0.30 * region * life

    accent = (h3 >= 0.93).astype(float)
    rim_i = np.maximum(rim, rim_u) * breath
    a_rim = rim_i * 0.52 * region * life
    rim_c = LINE[None, None, :] * (1 - accent[..., None]) + GLOW[None, None, :] * accent[..., None]

    red_blink = 0.5 + 0.5 * np.sin(time_s * 8.0)
    rim_warn = rim_c * (1 - warn) + WARN_C[None, None, :] * warn
    rim_alpha = a_rim * (1 - warn) + a_rim * (0.4 + 0.6 * red_blink) * warn

    edge_pulse = ((1.0 - smoothstep(0.0, 0.03, np.abs(length_c - blob_edge)))
                  * warn * (0.3 + 0.7 * red_blink) * 0.4)

    a = a_mem + a_body + a_crease + rim_alpha + edge_pulse
    rgb = (BG[None, None, :] * a_mem[..., None]
           + LINE[None, None, :] * a_body[..., None]
           + rim_warn * rim_alpha[..., None]
           + WARN_C[None, None, :] * edge_pulse[..., None])

    # premultiplied-over composite onto the stand-in wallpaper
    out = bg_image * (1.0 - a[..., None]) + rgb
    return np.clip(out, 0.0, 1.0)


def standin_wallpaper(width: int, height: int) -> np.ndarray:
    """Dark violet-gradient placeholder so transparency reads correctly."""
    yy = np.linspace(0.0, 1.0, height)[:, None]
    top = np.array([0.078, 0.067, 0.102])
    bot = np.array([0.125, 0.106, 0.157])
    grad = top[None, None, :] * (1 - yy[..., None]) + bot[None, None, :] * yy[..., None]
    img = np.broadcast_to(grad, (height, width, 3)).copy()
    rng = np.random.default_rng(7)
    img += rng.normal(0.0, 0.004, img.shape)  # faint grain, like a real wall
    return np.clip(img, 0.0, 1.0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/scales-preview")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=800)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    bg = standin_wallpaper(args.width, args.height)

    # (label, time_s, reveal, opening, warn) — mirrors the state machine
    states = [
        ("forming-1_2s", 1.2, min(1.2 / 2.5, 1.0), True, 0.0),
        ("idle-6s", 6.0, 1.0, True, 0.0),
        ("warn", 2.0, 1.0, True, 0.85),
        ("closing-0_4s", 0.4, max(0.0, 1.0 - 0.4 / 0.8), False, 0.0),
    ]

    tiles = []
    for label, t, reveal, opening, warn in states:
        frame = render_frame(args.width, args.height, time_s=t, reveal=reveal,
                             opening=opening, warn=warn, seed=12.34,
                             bg_image=bg)
        img = Image.fromarray((frame * 255).astype(np.uint8))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, args.width, 26], fill=(10, 10, 12))
        d.text((10, 7), label, fill=(232, 193, 90))
        path = os.path.join(args.out, f"{label}.png")
        img.save(path)
        tiles.append(img)
        print(f"wrote {path}")

    sheet = Image.new("RGB", (args.width, (args.height + 4) * len(tiles)))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, (0, i * (args.height + 4)))
    sheet_path = os.path.join(args.out, "sheet.png")
    sheet.save(sheet_path)
    print(f"wrote {sheet_path}")


if __name__ == "__main__":
    main()
