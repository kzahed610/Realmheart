// Broken Seal — Realmheart lockscreen.
// A torn patch of dragon-scale plating over the center of the screen (~20%
// of the area); the rest stays transparent so the wallpaper shows through.
//
// Material model (ref: Realmheart-Docs/Lockscreen/Scales.md — cloth panel):
//   each scale is a solid rounded plate of dark charcoal-plum material,
//   slightly LIFTED toward its center and falling into a DARK crease where
//   it overlaps its neighbors. The patch reads as one continuous dark cloth
//   surface cut into scales — not lines on transparency.
//   Palette: deep void base #17141c, plate face lifted toward lavender-grey
//   #5a5563, golden seam-glow #E8C15A breathing at the lattice nodes,
//   red #b3261e wrong-password flash.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;
in vec2 v_texcoord;

uniform vec2 uResolution;
uniform float uTime;
uniform float uProgress; // 0..1: forming (0->1) or closing (1->0)
uniform float uOpening;  // 1.0 = forming, 0.0 = closing
uniform float uReveal;   // 0..1: blob growth (forming) / erosion (closing)
uniform float uTarget;   // final blob radius in screen-height fractions (~0.34)
uniform float uWarn;     // 0..1: wrong-password flash
uniform float uSeed;
uniform float uLit;      // 0..1: fraction of scales lit by password input
uniform vec3 uBg;    // #17141c deep void
uniform vec3 uLine;  // #5a5563 lavender-grey plate lift
uniform vec3 uGlow;  // #E8C15A golden seam glow
uniform vec3 uWarnC; // #b3261e wrong-password red

layout(location = 0) out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.55;
    for (int i = 0; i < 3; i++) {
        v += noise2(p) * a;
        p = p * 2.13 + 17.0;
        a *= 0.5;
    }
    return v;
}
float ease_out_cubic(float t) {
    float f = 1.0 - t;
    return 1.0 - f * f * f;
}

void main() {
    vec2 uv = v_texcoord;
    vec2 aspect = vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
    vec2 c = (uv - 0.5) * aspect;

    // The torn central patch: an irregular fbm-torn blob growing to uTarget
    // radius (~20% of the screen area). Outside it, nothing renders.
    float radius = mix(0.05, uTarget, ease_out_cubic(uReveal));
    float tendril = fbm(c * 3.0 + uSeed) - 0.5;
    float blob_edge = radius * (1.0 + tendril * 0.35);

    // ---- The honeycomb lattice ----
    // Rotate by 45°; the lattice is the grid lines of that space (the
    // Scales.md "45° and -45° intersecting" pattern).
    // Vertical squash before rotation: cells render ~1.4x taller than wide
    // on screen, so scales read as elongated diamonds, not cushions.
    const float kSquash = 0.72; // on-screen scale height = cell / kSquash
    vec2 rot = mat2(0.7071, -0.7071, 0.7071, 0.7071)
        * (vec2(uv.x, uv.y * kSquash) * uResolution);
    float cell = 0.048 * uResolution.y;
    vec2 grid = rot / cell;
    vec2 cell_id = floor(grid);
    vec2 cell_p = fract(grid) - 0.5;

    // Cell center, back in the same normalized space as `c` — this is what
    // gates the region. (The pixel's own offset within its cell is NOT a
    // distance from the screen center.)
    vec2 rc = (cell_id + 0.5) * cell;
    vec2 pq = mat2(0.7071, 0.7071, -0.7071, 0.7071) * rc; // un-rotate
    vec2 cc = (vec2(pq.x, pq.y / kSquash) / uResolution - 0.5) * aspect;
    float cell_dist = length(cc);

    // Per-cell hash: variance + shimmer phase + reveal/erosion stagger.
    float h1 = hash12(cell_id + uSeed);
    float h2 = hash12(cell_id + 7.7 + uSeed);
    float h3 = hash12(cell_id + 31.3 + uSeed);
    float phase = h1 * 6.2831;
    float jitter = h2 * 0.25;

    // Gentle hand-drawn scallop: each line family bows slightly, so the
    // plates sit like soft chain-link scales rather than stamped tiles.
    float scallop = 0.020 * cell;
    float u = rot.x + scallop * sin(grid.y * 2.2 + uSeed * 11.0);
    float v = rot.y + scallop * cos(grid.x * 2.2 + uSeed * 7.0);

    // ---- Plate material (per cell) ----
    // Work in cell-local pixels. Each scale is a SOLID rounded diamond
    // plate: lit face falling into a dark overlap crease, sitting on the
    // dark membrane between plates. This is what makes it read as scales,
    // not wireframe diamonds.
    vec2 lp = vec2(u, v) - (cell_id + 0.5) * cell;
    // Rounded plate: an axis-aligned rounded BOX in LATTICE space. The 45°
    // lattice rotation turns it into a rounded DIAMOND on screen (the scale
    // silhouette), and the y-squash elongates it. A diamond SDF here would
    // rotate BACK to upright squares on screen — the exact bug this fixes.
    float half_sz = 0.475 * cell;                 // half-extent, tight seam gap
    float rb = 0.16 * cell;                       // corner rounding
    vec2 dq = abs(lp) - (half_sz - rb);
    float sd = length(max(dq, vec2(0.0))) + min(max(dq.x, dq.y), 0.0) - rb;

    // Thin dark seam right at the plate rim (where the next scale tucks
    // over it); the face stays lit toward the center.
    float crease = smoothstep(-0.05 * cell, -0.006 * cell, sd);
    // Plate body: inside the SDF (plates touch at the edge midpoints).
    float plate = 1.0 - smoothstep(-0.008 * cell, 0.008 * cell, sd);

    // Screen-space local offset (undo rotation, then undo the squash) so
    // the gradient runs top-to-bottom of the scale as seen on screen.
    vec2 lq = mat2(0.7071, 0.7071, -0.7071, 0.7071) * lp;
    vec2 lps = vec2(lq.x, lq.y / kSquash);
    float scale_h = cell / kSquash;
    float grad_t = clamp(0.5 - lps.y / scale_h, 0.0, 1.0);

    // Cloth folds: broad low-frequency drift across the patch + per-scale
    // variance. LOW contrast — the pattern whispers, like the reference.
    float mottle = (fbm(rot / (cell * 5.5) + uSeed * 5.0) - 0.5) * 0.14
                 + (h3 - 0.5) * 0.09;

    // Rare accent scales catch a little more light (~5% of plates).
    float accent = step(0.95, h1);

    const vec3 kMoss = vec3(0.282, 0.271, 0.243);     // #48453e moss cap
    const vec3 kBody = vec3(0.150, 0.150, 0.168);     // dark grey body
    vec3 cloth = kMoss * 0.52;                   // mid olive, opaque seams
    // Every scale carries its own gradient: body falls from grey (top) to
    // darker grey (bottom), then the moss caps the top ~25%.
    vec3 body = kBody * (0.75 + 0.55 * grad_t)
              * (0.92 + 0.20 * h3 + mottle * 0.5);
    float cap = smoothstep(0.68, 0.90, grad_t + (h3 - 0.5) * 0.05);
    vec3 plate_face = mix(body, kMoss, cap);
    plate_face = mix(plate_face, kMoss, 0.35 * accent);
    plate_face *= 1.0 - 0.30 * crease;           // rim seam darkens the face

    // Directional drop shadow, imbricate-style: light from top-left, so
    // each plate casts a soft crescent ONTO the plate below-right of it
    // (the overlap zone) and into the seam. Shadow-on-plate is what makes
    // real overlapping scales read as layered — shadow in a dark gap is
    // invisible. The shadow is the plate's own SDF sampled at a screen-
    // space offset (converted back to lattice space).
    float sh_m = 0.11 * cell;                    // offset magnitude
    vec2 sh_dir = normalize(vec2(0.55, 1.0));    // screen: down-right
    vec2 sh_lat = mat2(0.7071, -0.7071, 0.7071, 0.7071)
        * vec2(sh_dir.x * sh_m, sh_dir.y * sh_m * kSquash);
    vec2 sq2 = abs(lp - sh_lat) - (half_sz - rb);
    float sd_sh = length(max(sq2, vec2(0.0))) + min(max(sq2.x, sq2.y), 0.0) - rb;
    float scale_shadow = 1.0 - smoothstep(-0.07 * cell, 0.0, sd_sh);

    // Detail: a faint inner ridge following the plate contour, visible on
    // the top-lit face — the growth ring that makes plates read as scales
    // instead of blank tiles.
    vec2 dq_in = abs(lp) - (half_sz - rb - 0.16 * cell);
    float sd_in = length(max(dq_in, vec2(0.0)))
                + min(max(dq_in.x, dq_in.y), 0.0) - rb;
    float ridge = (1.0 - smoothstep(0.0, 0.035 * cell, abs(sd_in))) * plate;
    ridge *= 0.45 + 0.55 * clamp(0.5 - lps.y / scale_h, 0.0, 1.0);

    // ---- Life: staggered reveal / erosion (geometry unchanged) ----
    // Opening pops scales in ONE BY ONE: each cell transitions over a narrow
    // ~0.14 window (same snap as the closing dissolve), staggered by its
    // distance from the seed + per-cell jitter. No global fade wash.
    float reveal_here = smoothstep(0.44, 0.58, uReveal * 2.0 - cell_dist - jitter);
    float erosion = 1.0 - uReveal;
    float dissolve = smoothstep(cell_dist + jitter - 0.07,
                                cell_dist + jitter + 0.07, erosion);
    float life = uOpening > 0.5 ? reveal_here : 1.0 - dissolve;

    // Region: the cloth fades out over the rim band, but ONLY under the
    // rounded plate SDF — so rim cells show complete rounded scales with
    // wallpaper gaps between them. No cell-square silhouette, no hard
    // corners at the boundary; the edge is a ring of whole scales.
    float rim = 1.0 - smoothstep(blob_edge - 0.09, blob_edge, cell_dist);
    float region = rim * plate;

    // The patch: interior = continuous cloth + plates; rim = whole rounded
    // scales dissolving out (cloth only exists under plates there).
    float interior = 1.0 - smoothstep(blob_edge - 0.09, blob_edge - 0.02, cell_dist);
    float cloth_a = 0.96 * region * life;
    vec3 rgb = mix(cloth, plate_face, plate);
    rgb = mix(rgb, cloth * plate, 1.0 - interior);   // rim: olive under plates only
    rgb *= 1.0 - 0.60 * scale_shadow * region * life;
    // Ridge boost folds into the same rim-faded plate mask — rim plates
    // sink into the olive cloth instead of re-emerging via the ridge term.
    plate_face *= 1.0 + 0.28 * ridge;
    rgb = mix(rgb, plate_face, plate);
    float a = cloth_a;

    // ---- Keystroke ignition: the scales ARE the input bar ----
    // ONE scale per character, on the patch's horizontal center row. The
    // run is anchored at the MIDDLE and grows right-first: n chars light
    // columns [-floor((n-1)/2), +floor(n/2)] — so a short password lights
    // the center scales, never just the left edge. The newest char's scale
    // (the run's right end) pulses as the frontier.
    const float kLitChars = 12.0;                 // must match kMaxLitChars
    float n_chars = floor(uLit * kLitChars + 0.5);
    vec2 cc_px = cc * uResolution.y;              // normalized -> pixels
    float col_f = cc_px.x / (0.7071 * cell);      // lattice columns across
    float row_f = cc_px.y / (0.7071 * cell / kSquash); // lattice rows down
    float run_start = -floor((n_chars - 1.0) * 0.5);
    float run_end = floor(n_chars * 0.5);
    float in_run = step(run_start - 0.5, col_f) * step(col_f, run_end + 0.5);
    float in_row = 1.0 - smoothstep(0.45, 0.75, abs(row_f));
    float lit_active = step(0.001, uLit);
    float lit = lit_active * in_run * in_row;
    float frontier = lit_active * in_row
        * exp(-pow((col_f - run_end) * 2.0, 2.0));
    vec3 lit_col = mix(plate_face, uGlow, 0.45);
    rgb = mix(rgb, lit_col, lit * plate * 0.55 * region * life);
    rgb += uGlow * frontier * plate * 0.35 * region * life;

    // ---- Breathing seam glow at the lattice nodes ----
    // Gold light seeps through the tiny gaps where four plate corners meet
    // — the grid CORNER, not the cell center.
    vec2 gp = fract(vec2(u, v) / cell);
    vec2 nx = min(gp, 1.0 - gp);
    float node_dist = length(nx) * cell;
    float node_dot = 1.0 - smoothstep(0.0, 4.5, node_dist);
    float breath = 0.5 + 0.5 * sin(uTime * 1.5 + phase);
    float glow_a = node_dot * breath * 0.13 * region * life;

    // Occasional glints: a slow sheen sweeping diagonally across the cloth,
    // lighting one plate at a time as it passes.
    float sweep_t = mod(uTime / 11.0, 1.0);
    float sweep_pos = sweep_t * (aspect.x + 0.9) - 0.45;
    float sweep_band = exp(-pow((c.x + c.y * 0.6 - sweep_pos) * 5.5, 2.0));
    float glint_gate = step(0.75, h2);             // ~25% of scales glint
    // Shaped by the plate mask: scattered glints read as tiny rounded
    // diamonds drifting on the wallpaper (user-liked motes), not squares.
    // Deliberately NOT gated by region/life — they live outside the patch.
    float twinkle = sweep_band * glint_gate * 0.22 * plate;

    // Wrong-password: the SCALES flush red — plates tint toward uWarnC with
    // a slow pulse, seams blink red. The old edge halo is gone.
    float red_blink = 0.5 + 0.5 * sin(uTime * 8.0);
    vec3 seam_c = mix(uGlow, uWarnC, uWarn);
    float seam_a = mix(glow_a, glow_a * (0.4 + 0.6 * red_blink), uWarn);
    float warn_flush = uWarn * (0.55 + 0.20 * red_blink);
    rgb = mix(rgb, uWarnC, warn_flush * plate * region * life);
    rgb = mix(rgb, rgb * vec3(0.9, 0.45, 0.42), uWarn * 0.35);

    // Premultiplied output (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
    a += seam_a + twinkle * (1.0 - uWarn);
    rgb += uGlow * seam_a + plate_face * twinkle * (1.0 - uWarn);
    fragColor = vec4(rgb * a, a);
}
