// Broken Seal — "The Punch-Through" v4: smooth-silhouette raymarch edition.
// Lesson from the clay-stack incident: plates are SHADING, not geometry.
// The silhouette is one continuous smooth C-curl; armor ridges live only in
// normals + albedo so light breaks across them without deforming the form.
// True 3D swept-tube SDF, marched per-pixel during entrance/close only.
// 2D decal pass: breach cracks, torn flecks, contact shadow — calibrated
// for arbitrary (including bright) wallpapers.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform float progress;
uniform vec2 resolution;
uniform float opening;
uniform float uOpacity;

// Kept for the shared lockscreen contract; unused by this scene.
uniform float uSplit;
uniform float uAngle;
uniform float uOffsetX;
uniform float uOffsetY;
uniform vec3 uInterior;
uniform vec3 uVein;
uniform vec3 uEdge;

layout(location = 0) out vec4 fragColor;

// --- Palette ---
const vec3 kHornBlack = vec3(0.110, 0.110, 0.128);
const vec3 kPlateDark = vec3(0.045, 0.045, 0.056);
const vec3 kPlateLite = vec3(0.290, 0.285, 0.320);
const vec3 kRimViolet = vec3(0.780, 0.490, 1.0);
const vec3 kShadowCol = vec3(0.30, 0.29, 0.33);
const vec3 kCrackDark = vec3(0.16, 0.14, 0.19);
const vec3 kTornEdge  = vec3(0.72, 0.70, 0.78);

const float kPlates = 9.0;
const float kPi = 3.14159265;

// --- Left horn spine in 3D (y-down, z+ toward viewer).
// Smooth C-curl sweeping out-left and down, tip flicking outward-in at the
// end. Belly pushes toward the viewer mid-arc.
const int kSpineCount = 25;
const vec3 kSpine[25] = vec3[25](
    vec3(-0.1713, -0.2938, 0.020),
    vec3(-0.1876, -0.2866, 0.029),
    vec3(-0.2111, -0.2765, 0.042),
    vec3(-0.2368, -0.2680, 0.055),
    vec3(-0.2636, -0.2644, 0.067),
    vec3(-0.2925, -0.2623, 0.079),
    vec3(-0.3217, -0.2562, 0.091),
    vec3(-0.3530, -0.2446, 0.101),
    vec3(-0.3846, -0.2290, 0.110),
    vec3(-0.4087, -0.2078, 0.116),
    vec3(-0.4217, -0.1783, 0.119),
    vec3(-0.4271, -0.1431, 0.120),
    vec3(-0.4280, -0.1090, 0.117),
    vec3(-0.4221, -0.0782, 0.113),
    vec3(-0.4117, -0.0484, 0.106),
    vec3(-0.4066, -0.0199, 0.097),
    vec3(-0.4097, 0.0084, 0.087),
    vec3(-0.4180, 0.0354, 0.076),
    vec3(-0.4323, 0.0585, 0.064),
    vec3(-0.4543, 0.0775, 0.051),
    vec3(-0.4823, 0.0925, 0.036),
    vec3(-0.5118, 0.1015, 0.020),
    vec3(-0.5454, 0.1015, 0.020),
    vec3(-0.5805, 0.0955, 0.020),
    vec3(-0.6053, 0.0908, 0.020)
);
const float kWidths[25] = float[25](
    0.0644, 0.0648, 0.0660, 0.0673, 0.0680, 0.0734, 0.0810, 0.0835, 0.0790, 0.0747, 0.0725, 0.0650, 0.0602, 0.0577, 0.0528, 0.0505, 0.0469, 0.0400, 0.0356, 0.0328, 0.0248, 0.0194, 0.0156, 0.0072, 0.0043
);
const float kLens[25] = float[25](
    0.0000, 0.0247, 0.0602, 0.0978, 0.1352, 0.1755, 0.2168, 0.2631, 0.3120, 0.3565, 0.4013, 0.4507, 0.4980, 0.5415, 0.5852, 0.6255, 0.6649, 0.7042, 0.7419, 0.7822, 0.8262, 0.8690, 0.9156, 0.9650, 1.0000
);

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

// Distance to the SMOOTH tube (no geometric plate modulation).
float sdHorn3D(vec3 q, out float u, out vec3 radial, out vec3 tangent) {
    float best = 1e9;
    vec3 best_q = vec3(0.0);
    vec3 best_dir = vec3(1.0, 0.0, 0.0);
    u = 0.0;
    for (int i = 0; i < kSpineCount - 1; ++i) {
        vec3 a = kSpine[i];
        vec3 b = kSpine[i + 1];
        vec3 ab = b - a;
        float len2 = dot(ab, ab);
        float t = clamp(dot(q - a, ab) / len2, 0.0, 1.0);
        vec3 proj = a + ab * t;
        float d = length(q - proj);
        if (d < best) {
            best = d;
            best_q = proj;
            best_dir = ab;
            u = kLens[i] + t * (kLens[i + 1] - kLens[i]);
        }
    }
    vec3 to_q = q - best_q;
    tangent = normalize(best_dir);
    float tl = length(to_q);
    radial = tl > 1e-5 ? to_q / tl : vec3(0.0, 0.0, 1.0);
    return best - width_at(u);
}

bool in_horn_bounds(vec3 q) {
    vec3 c = vec3(-0.30, -0.06, 0.07);
    return dot(q - c, q - c) < 0.52 * 0.52;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash12(i), hash12(i + vec2(1.0, 0.0)), w.x),
        mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0)), w.x),
        w.y
    );
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.55;
    for (int octave = 0; octave < 4; ++octave) {
        value += noise2(p) * amplitude;
        p = p * 2.03 + vec2(9.7, 3.1);
        amplitude *= 0.5;
    }
    return value;
}

float crack_field(vec2 p, vec2 c, float spike_count, float seed) {
    vec2 d = p - c;
    float r = length(d);
    float ang = atan(d.y, d.x) + hash12(vec2(seed)) * 6.2831;
    // Per-spoke identity: irregular reach + thin spike profile, so the web
    // reads as shattered glass, not an even starburst.
    float k = ang / 6.2831 * spike_count;
    float kf = floor(k);
    float len_j = 0.30 + 0.70 * hash12(vec2(kf, seed * 13.1));
    float env = exp(-r * 7.5 / len_j) * step(r, len_j * 0.62 + 0.18);
    float profile = pow(0.5 + 0.5 * cos(fract(k) * 6.2831), 26.0);
    float jitter = fbm(p * 16.0 + seed) - 0.5;
    float line = profile * env * (0.75 + 0.5 * jitter);
    float mouth = exp(-r * 26.0);
    return clamp(line + mouth, 0.0, 1.0);
}

// Plate height profile: flat crown per segment, steep notch at each joint.
// Drives normal-only grooves (clay-stack lesson: never touch the silhouette).
float plate_h(float ph) {
    return smoothstep(0.04, 0.20, ph) * (1.0 - smoothstep(0.80, 0.96, ph));
}

void main() {
    vec2 uv = v_texcoord;
    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2 p = (uv - 0.5) * vec2(aspect, 1.0);

    float t_open = clamp(progress, 0.0, 1.0);
    bool closing = opening < 0.5;
    float alpha;
    float drive;
    float settle;
    float fracture;
    if (!closing) {
        alpha = smoothstep(0.0, 0.28, t_open);
        fracture = smoothstep(0.02, 0.45, t_open);
        drive = smoothstep(0.22, 0.92, t_open);
        settle = 1.0 + 0.08 * (1.0 - smoothstep(0.25, 0.95, t_open));
    } else {
        float tc = clamp(1.0 - progress, 0.0, 1.0);
        alpha = tc * tc;
        drive = tc * tc;
        settle = 1.0;
        fracture = tc * tc;
    }

    vec2 breach_l = vec2(-0.118, -0.272);
    vec2 breach_r = vec2(0.118, -0.272);
    vec2 ps = breach_l + (p - breach_l) * settle;

    // Camera: gentle perspective down -z.
    vec3 ro = vec3(ps * 0.92, 1.35);
    vec3 rd = normalize(vec3(ps * 0.10, -1.0));
    vec3 ro_r = vec3(-ro.x, ro.y, ro.z);
    vec3 rd_r = vec3(-rd.x, rd.y, rd.z);

    // --- Raymarch both horns ---
    float u_hit = 0.0;
    vec3 n_hit = vec3(0.0, 0.0, 1.0);
    float t_hit = -1.0;
    for (int side = 0; side < 2; ++side) {
        vec3 o = side == 0 ? ro : ro_r;
        vec3 d = side == 0 ? rd : rd_r;
        if (!in_horn_bounds(o) &&
            !in_horn_bounds(o + d * 0.5) &&
            !in_horn_bounds(o + d * 1.2)) {
            continue;
        }
        float t = 0.0;
        for (int step_i = 0; step_i < 44; ++step_i) {
            vec3 q = o + d * t;
            float uu;
            vec3 rad; vec3 tan;
            float dist = sdHorn3D(q, uu, rad, tan);
            dist = max(dist, (uu - drive) * 0.05);
            if (dist < 0.0022) {
                // Both horns always march: the nearer surface wins, so the
                // decal pass can be occlusion-masked by the TRUE silhouette.
                if (t_hit < 0.0 || t < t_hit) {
                    t_hit = t;
                    u_hit = uu;
                    const vec2 e = vec2(0.004, -0.004);
                    float u1; vec3 r1; vec3 t1;
                    float u2; vec3 r2; vec3 t2;
                    float u3; vec3 r3; vec3 t3;
                    float u4; vec3 r4; vec3 t4;
                    float d1 = sdHorn3D(q + e.xyy * 1.5, u1, r1, t1);
                    float d2 = sdHorn3D(q + e.yyx * 1.5, u2, r2, t2);
                    float d3 = sdHorn3D(q + e.yxy * 1.5, u3, r3, t3);
                    float d4 = sdHorn3D(q + e.xxx * 1.5, u4, r4, t4);
                    n_hit = normalize(vec3(d1 - d2, d3 - d4, d1 + d2 - (d3 + d4)));
                }
                break;
            }
            t += dist * 0.85;
            if (t > 2.6) break;
        }
    }

    // --- Horn shading: smooth body, plates as LIGHT only ---
    vec3 colour = vec3(0.0);
    float horn_alpha = 0.0;
    if (t_hit > 0.0) {
        horn_alpha = alpha;
        vec3 light_dir = normalize(vec3(0.55, -0.45, 0.70));
        float diff = max(dot(n_hit, light_dir), 0.0);

        float phase = fract(u_hit * kPlates);
        // Re-derive the true tube tangent at the hit point (stable at
        // silhouettes, unlike screen-space derivatives).
        float u_t; vec3 rad_t; vec3 tan_t;
        vec3 q_hit = ro + rd * t_hit;
        if (ro.x < 0.0) { sdHorn3D(q_hit, u_t, rad_t, tan_t); }
        else { sdHorn3D(vec3(-q_hit.x, q_hit.yz), u_t, rad_t, tan_t);
               tan_t = vec3(-tan_t.x, tan_t.yz); }

        // Armor joints: analytic slope of the plateau profile tilts the
        // normal into a hard notch-and-lip at each plate boundary.
        float pe = 0.015;
        bool at_seam = phase <= pe;
        float dh = (plate_h(phase + pe)
            - plate_h(at_seam ? phase : phase - pe))
            / (at_seam ? pe : 2.0 * pe);
        n_hit.xy -= normalize(tan_t.xy + 1e-5) * dh * 0.60;
        n_hit = normalize(n_hit);

        // Tight dark seam ring exactly at the joint; near-uniform plate tones.
        float joint = 1.0 - smoothstep(0.006, 0.036, min(phase, 1.0 - phase));
        float plate_tone = 0.95 + 0.08 * hash12(vec2(floor(u_hit * kPlates), 7.0));
        vec3 albedo = mix(kHornBlack, kPlateDark, joint * 0.85);

        // Narrow satin sheen hugging the crest that faces the key light.
        float crest = pow(smoothstep(0.10, 0.95,
            dot(normalize(n_hit.xy + vec2(1e-4)), normalize(light_dir.xy))), 1.6);
        albedo = mix(albedo, kPlateLite, crest * 0.22);

        float ao = mix(0.85, 1.0, clamp(n_hit.y * -0.5 + 0.5, 0.0, 1.0));
        ao *= 1.0 - 0.28 * joint;
        vec3 half_dir = normalize(light_dir + vec3(0.0, 0.0, 1.0));
        float spec = pow(max(dot(n_hit, half_dir), 0.0), 64.0) * 0.22
            * (1.0 - joint);
        colour = albedo * plate_tone * ao * (0.42 + diff * 1.05);
        colour += kPlateLite * spec;
        float fresnel = pow(1.0 - clamp(n_hit.z, 0.0, 1.0), 4.0);
        colour += kRimViolet * fresnel * 0.02;
        colour = colour * 1.30 + vec3(0.004);
    }

    // --- 2D decal pass on the glass plane ---
    // Decals never paint over the horn bodies: body_free masks everything.
    float body_free = 1.0 - horn_alpha;
    vec2 pcx = p - vec2(0.0, 0.02);
    float web_l = crack_field(pcx, breach_l, 11.0, 1.7);
    float web_r = crack_field(vec2(-pcx.x, pcx.y), breach_r, 11.0, 5.3);
    float web = clamp(web_l + web_r, 0.0, 1.0) * fracture * body_free;

    float fleck_zone_l = exp(-length(pcx - breach_l) * 20.0);
    float fleck_zone_r = exp(-length(vec2(-pcx.x, pcx.y) - breach_r) * 20.0);
    float flecks = clamp(fleck_zone_l + fleck_zone_r, 0.0, 1.0)
        * step(0.64, fbm(pcx * 26.0 + 4.2)) * fracture * 0.52 * body_free;

    // Cracks read as DEEP gouges: premultiplied near-black core plus a thin
    // pale chip-highlight pair hugging each crack edge (any wallpaper).
    float core = smoothstep(0.30, 0.58, web);
    float chip = smoothstep(0.16, 0.26, web) * (1.0 - smoothstep(0.30, 0.52, web));

    vec2 shadow_off = vec2(-0.07, 0.09);
    vec2 sd_l = (pcx - shadow_off - vec2(-0.24, 0.02)) * vec2(1.30, 0.78);
    vec2 sd_r = (pcx - shadow_off - vec2(0.24, 0.02)) * vec2(1.30, 0.78);
    float sh_l = exp(-length(sd_l) * 4.4);
    float sh_r = exp(-length(sd_r) * 4.4);
    float cast_shadow = clamp(sh_l + sh_r, 0.0, 1.0) * 0.36
        * body_free * smoothstep(0.30, 0.85, drive);

    // --- Composite premultiplied ---
    float crack_a = clamp(core * 0.82 + flecks, 0.0, 1.0);
    float shadow_a = clamp(cast_shadow, 0.0, 1.0);
    float alpha_out = clamp(
        horn_alpha + crack_a * alpha + shadow_a * alpha,
        0.0, 1.0
    ) * uOpacity;
    vec3 rgb = colour
        + kCrackDark * core * 0.60      // deep gouge core, punches via alpha
        + kTornEdge * chip * 0.55       // chipped-edge highlight pair
        + kTornEdge * flecks
        + kShadowCol * cast_shadow;

    fragColor = vec4(rgb * alpha_out, alpha_out);
}
