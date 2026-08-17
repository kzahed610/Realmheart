// White Core Smoke — TBATE Lore-Accurate Mana Core Ethereal Mist
// Billowing pure white and silver smoke orbiting the White Mana Core.
// Smoothly wraps the core boundary with organic FBM turbulence and micro-motes,
// strictly masking the inner circle to preserve full wallpaper clarity.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform float u_time;
uniform vec2  u_resolution;
uniform vec2  u_core_center;
uniform float u_core_radius;
uniform float u_alpha;
uniform float u_heartbeat;

layout(location = 0) out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
        mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.55;
    for (int i = 0; i < 3; i++) {
        v += noise2(p) * a;
        p = p * 2.15 + 13.37;
        a *= 0.5;
    }
    return v;
}

void main() {
    if (u_alpha <= 0.001 || u_core_radius <= 2.0) {
        fragColor = vec4(0.0);
        return;
    }

    vec2 pixelPos = v_texcoord * u_resolution;
    vec2 d = pixelPos - u_core_center;
    float r = length(d);

    // Completely transparent inside the inner core circle to ensure zero wallpaper distortion
    if (r < u_core_radius - 2.0) {
        fragColor = vec4(0.0);
        return;
    }

    float scale = u_resolution.y / 1080.0;
    float angle = (r > 0.0) ? atan(d.y, d.x) : 0.0;

    // Smooth transition right at the core rim
    float inner_mask = smoothstep(u_core_radius - 1.0, u_core_radius + 4.0 * scale, r);

    // Multi-octave organic turbulence at the core boundary
    vec2 polar_uv = vec2(cos(angle) * 3.2 + u_time * 0.22, sin(angle) * 3.2 - u_time * 0.18);
    float boundary_turb = (fbm(polar_uv) - 0.5) * (14.0 * scale);
    float distorted_edge = u_core_radius + boundary_turb;

    // Radial smoke envelope hugging the core
    float dEdge = r - distorted_edge;
    float band_width = (48.0 + u_heartbeat * 18.0) * scale;
    float env = exp(-pow(max(0.0, dEdge - band_width * 0.15) / (band_width * 0.45), 2.0));

    // Orbital swirling smoke (volumetric billows rolling along the rim)
    vec2 swirl_coord = vec2(angle * 2.2 + u_time * 0.30, (r - u_core_radius) * 0.035 - u_time * 0.15);
    float dens1 = fbm(swirl_coord * 2.5 + vec2(3.1, 7.8));
    float dens2 = fbm(vec2(d.x * 0.018 + u_time * 0.12, d.y * 0.018 - u_time * 0.10));
    float smoke = clamp(env * (dens1 * 1.5 + dens2 * 0.8), 0.0, 1.0);

    // Ethereal wisps trailing outwards
    vec2 wisp_coord = vec2(angle * 4.5 - u_time * 0.45, (r - u_core_radius) * 0.022);
    float wisps = pow(fbm(wisp_coord * 2.8), 2.0) * exp(-max(0.0, r - u_core_radius) / (band_width * 1.5));

    // Radiant inner white rim glow
    float rim_glow = exp(-max(0.0, r - u_core_radius) / (12.0 * scale)) * (0.85 + 0.40 * u_heartbeat);

    // Luminous micro-motes of pure mana glittering in the smoke
    vec2 mote_uv = pixelPos / (26.0 * scale);
    vec2 mote_id = floor(mote_uv);
    float mote_hash = hash12(mote_id + 5.31);
    vec2 mote_subpos = mote_id + 0.2 + 0.6 * vec2(hash12(mote_id + 1.7), hash12(mote_id + 9.3));
    float mote_d2 = dot(mote_uv - mote_subpos, mote_uv - mote_subpos);
    float twinkle = 0.5 + 0.5 * sin(u_time * 5.0 + mote_hash * 30.0);
    float mote = step(0.76, mote_hash) * exp(-mote_d2 * 55.0) * smoke * twinkle * (0.8 + 0.4 * u_heartbeat);

    // Pure White / Silver / Grey palette (TBATE Lore-Accurate White Core)
    vec3 col_pure_white = vec3(1.0, 1.0, 1.0);
    vec3 col_silver = vec3(0.86, 0.90, 0.95);
    vec3 col_slate = vec3(0.65, 0.70, 0.76);

    // Blend colours from core white to outer silver/slate smoke
    vec3 smoke_color = mix(col_slate, col_silver, smoothstep(0.1, 0.6, smoke));
    smoke_color = mix(smoke_color, col_pure_white, rim_glow * 0.8 + mote * 1.0);

    // Total alpha computation
    float combined_alpha = clamp((smoke * 0.90 + wisps * 0.55 + rim_glow * 0.75 + mote * 1.2) * inner_mask * u_alpha, 0.0, 1.0);

    // Premultiplied alpha output for OpenGL compositing
    fragColor = vec4(smoke_color * combined_alpha, combined_alpha);
}
