// Broken Seal — horn pair.
// A pair of symmetrical, large ram-like horns that emerge from the upper
// sides, thick at the base, arcing outward and downward, with sharp tips
// curling inward. Dark matte obsidian with a subtle cool purplish tint and
// ribbed transverse ridges. The horns are fixed (no split/slide); the
// password is typed in the gap between them. No link between the horns.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform float progress;
uniform vec2 resolution;
uniform float opening;
uniform float uOpacity;

// Kept for the shared lockscreen contract; the horns do not split.
uniform float uSplit;
uniform float uAngle;
uniform float uOffsetX;
uniform float uOffsetY;

uniform vec3 uInterior;
uniform vec3 uVein;
uniform vec3 uEdge;

layout(location = 0) out vec4 fragColor;

// --- Horn palette (from the Horns.md spec) ---
const vec3 kBaseColour = vec3(0.169, 0.169, 0.196);      // #2B2B32 mid-tone
const vec3 kShadowColour = vec3(0.102, 0.102, 0.118);    // #1A1A1E deep crevice
const vec3 kHighlightColour = vec3(0.298, 0.298, 0.353); // #4C4C5A satin sheen
const vec3 kRimColour = vec3(0.780, 0.490, 1.0);         // faint aether violet

// --- Left horn spine (y-down space: base at top, tip at bottom) ---
// The spine is a polyline from the thick base, sweeping outward and down,
// then hooking inward at the tip. The right horn is its mirror image.
const int kSpineCount = 6;
const vec2 kSpine[6] = vec2[6](
    vec2(-0.16, -0.36), // base (thick, top)
    vec2(-0.36, -0.22), // sweep outward-left
    vec2(-0.42,  0.00), // far left, mid height
    vec2(-0.34,  0.18), // curving back inward
    vec2(-0.22,  0.28), // hook
    vec2(-0.10,  0.30)  // tip (razor, curled inward-down)
);
// Horn width along the spine: thick base -> razor tip.
const float kWidths[6] = float[6](0.100, 0.078, 0.055, 0.032, 0.015, 0.006);
// Normalized cumulative arc-length at each spine sample.
const float kLens[6] = float[6](0.0, 0.258, 0.499, 0.707, 0.872, 1.0);

// Interpolated horn width at normalized arc position u (0 = base, 1 = tip),
// with smoothstep easing between knots so the taper never shows a seam.
float width_at(float u) {
    for (int i = 0; i < kSpineCount - 1; ++i) {
        if (u <= kLens[i + 1]) {
            float seg = max(kLens[i + 1] - kLens[i], 1e-5);
            float t = clamp((u - kLens[i]) / seg, 0.0, 1.0);
            float s = t * t * (3.0 - 2.0 * t);
            return mix(kWidths[i], kWidths[i + 1], s);
        }
    }
    return kWidths[kSpineCount - 1];
}

// Signed distance to the tapered horn spine. Also outputs the normalized arc
// position u (for ribs/highlights) and side (+1 = outer curve for the left
// horn, -1 = inner).
// The spine is a polyline (keeping the horn's segmented, organic character).
// To avoid the boxy inner-corner artifact where the thick base meets the next
// segment, the width grows slightly near each interior joint on the inner
// (concave) side only, blending the sections while preserving the outer
// silhouette.
float sdHorn(vec2 p, out float u, out float side) {
    float best = 1e9;
    vec2 best_q = vec2(0.0);
    vec2 best_dir = vec2(1.0, 0.0);
    for (int i = 0; i < kSpineCount - 1; ++i) {
        vec2 a = kSpine[i];
        vec2 b = kSpine[i + 1];
        vec2 ab = b - a;
        float len2 = dot(ab, ab);
        float t = clamp(dot(p - a, ab) / len2, 0.0, 1.0);
        vec2 q = a + ab * t;
        float d = length(p - q);
        if (d < best) {
            best = d;
            best_q = q;
            best_dir = ab;
            u = kLens[i] + t * (kLens[i + 1] - kLens[i]);
        }
    }
    vec2 to_p = p - best_q;
    side = sign(to_p.x * best_dir.y - to_p.y * best_dir.x);
    float width = width_at(u);

    // Round the inner (concave) corner: grow the width slightly near the
    // spine joints on the inner side only, so the sections blend into each
    // other instead of leaving a boxy bulge. The outer curve keeps its exact
    // polyline silhouette.
    float joint = 1.0;
    joint = min(joint, abs(u - 0.258));
    joint = min(joint, abs(u - 0.499));
    joint = min(joint, abs(u - 0.707));
    joint = min(joint, abs(u - 0.872));
    joint = 1.0 - smoothstep(0.0, 0.09, joint);
    float inner = smoothstep(0.0, 0.02, -side);
    width *= 1.0 + joint * inner * 0.10;

    return best - width;
}

// Material for one horn: obsidian base, ribbed transverse ridges, satin
// highlight on the outer/upper curve. No colored rim.
vec3 shade_horn(float u, float side, float d) {
    vec3 colour = kBaseColour;

    // Transverse ribs (~10 ridges along the horn).
    float rib = 0.5 + 0.5 * sin(u * 62.8319);
    float ridge = smoothstep(0.35, 0.8, rib);
    colour = mix(colour, kShadowColour, ridge * 0.60);
    colour = mix(colour, kShadowColour, (1.0 - smoothstep(0.2, 0.6, rib)) * 0.30);

    // Satin highlight on the outer curve, stronger near the base.
    float outer = smoothstep(0.0, 0.06, side);
    float near_base = 1.0 - smoothstep(0.10, 0.55, u);
    colour = mix(colour, kHighlightColour, outer * (0.25 + 0.45 * near_base));

    return colour;
}

void main() {
    vec2 uv = v_texcoord;
    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2 p = (uv - 0.5) * vec2(aspect, 1.0);

    // Emergence: the horns fade in while they grow from base to tip (the
    // reveal below). No scale pop — the growth IS the emergence.
    float scale = 1.0;
    float alpha = 1.0;
    if (opening > 0.5) {
        float t = clamp(progress, 0.0, 1.0);
        alpha = smoothstep(0.0, 0.35, t);
    } else {
        float t = clamp(1.0 - progress, 0.0, 1.0);
        scale = mix(0.2, 1.0, t * t);
        alpha = t * t;
    }
    p /= max(scale, 0.001);

    // Growth reveal: the horns form from the base (u=0) toward the tip (u=1)
    // during the Opening phase, so it looks like they grow out of the top.
    // Outside Opening the horns are fully grown.
    float reveal = 1.0;
    if (opening > 0.5) {
        float t = clamp(progress, 0.0, 1.0);
        reveal = smoothstep(0.05, 0.95, t);
    }

    // Left horn + mirrored right horn.
    float u_l;
    float side_l;
    float dl = sdHorn(p, u_l, side_l);

    vec2 mp = vec2(-p.x, p.y);
    float u_r;
    float side_r;
    float dr = sdHorn(mp, u_r, side_r);

    // Only show the portion of each horn that has grown so far.
    dl = max(dl, (u_l - reveal) * 0.02);
    dr = max(dr, (u_r - reveal) * 0.02);

    float d_eff = min(dl, dr);

    // Shade each horn in its own space; blend by whichever is closer.
    vec3 colour_l = shade_horn(u_l, side_l, dl);
    vec3 colour_r = shade_horn(u_r, side_r, dr);
    float left_weight = 1.0 - smoothstep(0.0, 0.02, dl - dr);
    vec3 colour = mix(colour_r, colour_l, left_weight);

    // Hard silhouette with a faint dark halo.
    float inside = 1.0 - smoothstep(0.0, 0.006, d_eff);
    float halo = exp(-max(d_eff, 0.0) * 2.4) * 0.12;
    vec3 halo_colour = kShadowColour;

    float alpha_out = clamp(inside + halo, 0.0, 1.0) * alpha * uOpacity;
    vec3 rgb = colour * inside + halo_colour * halo;

    // Premultiplied alpha for clean compositing over the blurred desktop.
    fragColor = vec4(rgb * alpha_out, alpha_out);
}
