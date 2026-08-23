// Broken Seal — Realmheart lockscreen.
// A living field of scales grows over the center of the screen, breathes in
// idle, flashes red on a wrong password, and erodes on unlock. The password
// entry and "BROKEN SEAL" title are GTK widgets above this GL surface.
// The field is ref-faithful to the Scales.md pattern: a fine diagonal lattice
// of rounded diamonds, faint lavender-grey on the dark aether base, with a
// gentle hand-drawn scallop. No quilted fabric lighting — the breathing seam
// glow is the only lighting on the pattern.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;
in vec2 v_texcoord;

uniform vec2 uResolution;
uniform float uTime;
uniform float uProgress; // 0..1: forming (0->1) or closing (1->0)
uniform float uOpening;  // 1.0 = forming, 0.0 = closing
uniform float uReveal;   // 0..1: how far the blob has grown (authoritative)
uniform float uTarget;   // final blob coverage, ~0.20
uniform float uWarn;     // 0..1: wrong-password flash
uniform float uSeed;
uniform vec3 uBg;    // #17141c deep void
uniform vec3 uLine;  // #5a5563 lavender-grey scale lattice
uniform vec3 uGlow;  // #E8C15A golden seam/breath glow
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

// Rounded-diamond SDF in diamond-local space (|x|+|y| <= 1, rounded by r).
float rounded_diamond(vec2 p, float r) {
    float d = (abs(p.x) + abs(p.y) - 1.0 + r) / (1.0 + r);
    return length(max(vec2(d, 0.0), vec2(0.0))) + min(max(d, 0.0), 0.0) - r;
}

void main() {
    vec2 uv = v_texcoord;
    vec2 aspect = vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
    vec2 c = (uv - 0.5) * aspect;

    // The forming/eroding blob: an irregular fbm-torn seed that grows from
    // ~4 scales to uTarget screen coverage. Everything outside stays fully
    // transparent so the wallpaper shows through. Radius is in screen-height
    // fractions, so uTarget 0.20 ≈ a circle covering ~20% of the screen
    // height; the fbm-torn edge keeps it from reading as a perfect circle.
    float target = uTarget;
    float radius = mix(0.04, target, ease_out_cubic(uReveal));
    float tendril = fbm(c * 3.0 + uSeed) - 0.5;
    float blob_edge = radius * (1.0 + tendril * 0.35);
    float region_edge = smoothstep(blob_edge, blob_edge + 0.025, length(c));

    // ---- The scale field ----
    // Rotate UV by 45° and build a diamond grid. Each cell gets hash-derived
    // size/phase/drift so scales don't look stamped.
    vec2 rotated = mat2(0.7071, -0.7071, 0.7071, 0.7071) * (uv * uResolution);
    float cell = 0.045 * uResolution.y;
    vec2 grid = rotated / cell;
    vec2 cell_id = floor(grid);
    vec2 cell_p = fract(grid) - 0.5;

    float h1 = hash12(cell_id + uSeed);
    float h2 = hash12(cell_id + 7.7 + uSeed);
    float h3 = hash12(cell_id + 31.3 + uSeed);
    float size_v = 0.55 + 0.45 * h1;
    float phase = h3 * 6.2831;

    // Gentle hand-drawn scallop: a low-frequency wobble bends the lattice
    // slightly (a soft chain-link curvature, NOT a fabric-tension warp).
    vec2 warp = 0.015 * vec2(
        sin(grid.y * 1.7 + uSeed * 3.1),
        cos(grid.x * 1.7 + uSeed * 1.3)
    );

    vec2 p = (cell_p + warp) / (size_v * 0.5);
    float d = rounded_diamond(p, 0.09);

    // Edges are thin (~1px), soft, faint at rest.
    float line_w = 1.5 / cell;
    float edge = smoothstep(line_w * 0.5, -line_w * 0.5, d);
    float line_alpha = edge * 0.13;

    // ---- Region gate: the pattern lives only inside the central blob ----
    // Cell-center distance from the blob center; the whole cell is gated by
    // its own position, so no stray scales can appear outside the region.
    float cell_dist = length(cell_p * cell / max(uResolution.y, 1.0)) * 2.0;
    float region = 1.0 - smoothstep(blob_edge, blob_edge + 0.06, cell_dist);
    line_alpha *= region;
    // The seam glow follows the same gate so no gold lines can exist outside
    // the blob either.
    float seam_alpha = region;

    // Fading with reveal: scales appear one by one and interconnect, ordered
    // by their distance from the seed point. Only scales whose cell lies
    // inside the region can fade in.
    float reveal_here = smoothstep(0.15, 0.85, uReveal * 2.0 - cell_dist);
    line_alpha *= reveal_here;

    // Per-scale shimmer: a slow global drift plus a faint opacity oscillation.
    float shimmer = 0.75 + 0.25 * sin(uTime * 1.2 + phase);
    line_alpha *= shimmer;

    // A few outlying scales inside the blob, ahead of its edge, matching the
    // "scales increase in random areas" feel of the reference. Hash-gated to
    // ~2% of cells so they read as sparse, not full-screen.
    float fringe = 0.0;
    if (uReveal > 0.55) {
        float far = length((fract(grid) - 0.5) * 2.0);
        float chance = smoothstep(0.55, 1.0, uReveal) * step(0.985, h1);
        fringe = step(far, 0.30) * chance;
    }
    line_alpha = mix(line_alpha, 0.13 * shimmer, fringe * region);

    // ---- The seam glow ----
    // Where a scale edge meets its neighbor, a thin gold line breathes.
    float seam_band = 1.0 - smoothstep(0.0, 0.02, abs(d));
    float breath = 0.5 + 0.5 * sin(uTime * 1.5 + phase);
    float glow_a = seam_band * breath * 0.10 * seam_alpha;

    // Wrong-password: the seam glow turns red and blinks; a red flash pulse
    // rings along the blob edge.
    float red_blink = 0.5 + 0.5 * sin(uTime * 8.0);
    vec3 seam_c = mix(uGlow, uWarnC, uWarn);
    float seam_a = mix(glow_a, glow_a * (0.4 + 0.6 * red_blink), uWarn);

    float edge_pulse = smoothstep(0.02, 0.0, abs(length(c) - blob_edge)) *
        uWarn * (0.3 + 0.7 * red_blink) * 0.5 * region_edge;

    // Premultiplied output (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
    float alpha = line_alpha + seam_a + edge_pulse;
    vec3 rgb = uLine * line_alpha +
               seam_c * (seam_a + edge_pulse) +
               uBg * (1.0 - alpha);
    fragColor = vec4(rgb * alpha, alpha);
}
