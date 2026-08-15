// Elemental workspace-overview frontier enhancement.
// Geometry owns the rune/rail/band morph below this transparent layer. This
// shader only adds per-element breakup, ignition and particles around the
// exact frontier positions supplied by that geometry model.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform float progress;
uniform float opening;
uniform vec2 resolution;
uniform sampler2D tex;
uniform vec2 origin;
uniform vec4 elementStyle;
uniform vec4 sourceY;
uniform vec4 revealLeftX;
uniform vec4 frontX;
uniform vec4 frontTop;
uniform vec4 frontBottom;
uniform vec3 uGold;
uniform vec3 uStarlight;
uniform vec3 uAstral;
uniform vec3 uVoid;

layout(location = 0) out vec4 fragColor;

float hash21(vec2 value) {
    value = fract(value * vec2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return fract(value.x * value.y);
}

float value_noise(vec2 value) {
    vec2 cell = floor(value);
    vec2 local = fract(value);
    local = local * local * (3.0 - 2.0 * local);
    float a = hash21(cell);
    float b = hash21(cell + vec2(1.0, 0.0));
    float c = hash21(cell + vec2(0.0, 1.0));
    float d = hash21(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float component(vec4 values, int index) {
    if (index == 0) return values.x;
    if (index == 1) return values.y;
    if (index == 2) return values.z;
    return values.w;
}

int band_index(float y) {
    int best = -1;
    float best_distance = 2.0;
    for (int index = 0; index < 4; ++index) {
        float top = component(frontTop, index);
        float bottom = component(frontBottom, index);
        if (y < top || y > bottom) continue;
        float distance_to_source = abs(y - component(sourceY, index));
        if (best < 0 || distance_to_source < best_distance) {
            best = index;
            best_distance = distance_to_source;
        }
    }
    return best;
}

vec3 elemental_colour(float style) {
    if (style < 0.5) return mix(vec3(1.0, 0.18, 0.035), uGold, 0.28);
    if (style < 1.5) return mix(vec3(0.10, 0.72, 1.0), uStarlight, 0.34);
    if (style < 2.5) return mix(vec3(0.30, 0.92, 0.48), uStarlight, 0.12);
    return mix(vec3(0.92, 0.62, 0.20), uGold, 0.62);
}

void main() {
    vec2 uv = v_texcoord;

    if (progress <= 0.0005) {
        fragColor = vec4(0.0);
        return;
    }
    if (progress >= 0.9995) {
        fragColor = texture(tex, uv);
        return;
    }

    int band = band_index(uv.y);
    if (band < 0) {
        fragColor = vec4(0.0);
        return;
    }

    float style = component(elementStyle, band);
    float left = component(revealLeftX, band);
    float front = component(frontX, band);
    float top = component(frontTop, band);
    float bottom = component(frontBottom, band);
    float rune_y = component(sourceY, band);
    float direction = opening > 0.5 ? 1.0 : -1.0;
    float pixel_x = 1.0 / max(resolution.x, 1.0);
    float pixel_y = 1.0 / max(resolution.y, 1.0);
    float local_band_height = max(bottom - top, pixel_y);

    float ignition_age = smoothstep(0.02, 0.22, progress) *
        (1.0 - smoothstep(0.20, 0.46, progress));
    vec2 ignition_delta = vec2(
        (uv.x - origin.x) * resolution.x,
        (uv.y - rune_y) * resolution.y
    );

    // The effect occupies one moving frontier strip plus a short-lived rune
    // ignition. Once the first shader frame is ready, native geometry stops
    // slightly behind this strip so the procedural edge owns the visible
    // cutoff instead of merely glowing over a straight rectangle.
    if ((uv.x < left - 0.015 || abs(uv.x - front) > 0.095) &&
        (ignition_age <= 0.001 || length(ignition_delta) > 96.0)) {
        fragColor = vec4(0.0);
        return;
    }

    vec2 noise_space = vec2(
        uv.x * 54.0 + progress * 8.0 * direction,
        uv.y * 32.0 - progress * 4.0 * direction
    );
    float broad_noise = value_noise(noise_space);
    float fine_noise = value_noise(noise_space * 2.7 + 17.0);
    float breakup = (broad_noise - 0.5) * 0.016;
    float displacement = 0.0;
    float edge_width = 30.0 * pixel_x;

    if (style < 0.5) {
        breakup += (fine_noise - 0.5) * 0.020;
        displacement = sin(uv.y * 120.0 + progress * 18.0) * 2.4 * pixel_x;
        edge_width = 36.0 * pixel_x;
    } else if (style < 1.5) {
        breakup += sin(uv.y * 48.0 + progress * 10.0) * 0.006;
        displacement = sin(uv.y * 80.0 - progress * 12.0) * 4.0 * pixel_x;
        edge_width = 46.0 * pixel_x;
    } else if (style < 2.5) {
        breakup += sin(uv.y * 92.0 + broad_noise * 8.0) * 0.010;
        displacement = (broad_noise - 0.5) * 5.0 * pixel_x;
        edge_width = 40.0 * pixel_x;
    } else {
        float fractured = floor(fine_noise * 5.0) / 5.0;
        breakup += (fractured - 0.5) * 0.016;
        displacement = (fractured - 0.5) * 2.2 * pixel_x;
        edge_width = 38.0 * pixel_x;
    }

    float unperturbed_distance = uv.x - front;
    float signed_distance = unperturbed_distance + breakup;
    float frontier = exp(-abs(signed_distance) / max(edge_width, pixel_x));

    // Cover the 52 px native inset with the captured scene, then let the
    // noise-perturbed frontier decide where that scene dissolves into
    // transparency. A short overlap behind the inset prevents pinholes while
    // remaining narrow enough to avoid visible double-compositing.
    float source_trail = smoothstep(
        -76.0 * pixel_x,
        -44.0 * pixel_x,
        unperturbed_distance
    );
    float materialized = 1.0 - smoothstep(
        -edge_width * 0.55,
        edge_width * 0.78,
        signed_distance
    );
    float root_mask = smoothstep(left - pixel_x * 2.0, left + pixel_x * 4.0, uv.x);
    float source_mask = source_trail * materialized * root_mask;

    vec2 displaced_uv = clamp(
        uv + vec2(displacement * frontier, 0.0),
        vec2(0.0),
        vec2(1.0)
    );
    vec4 source = texture(tex, displaced_uv);

    vec3 element = elemental_colour(style);
    float glow_alpha = frontier * mix(0.34, 0.62, fine_noise);
    float hot_core = exp(
        -abs(signed_distance) / max(edge_width * 0.30, pixel_x)
    );
    glow_alpha += hot_core * 0.34;

    float ignition = exp(-length(ignition_delta) / 42.0) * ignition_age;

    vec2 particle_cell = floor(vec2(
        uv.x * resolution.x / 7.0,
        uv.y * resolution.y / 7.0
    ));
    float particle_seed = hash21(
        particle_cell + vec2(float(band) * 19.0, floor(progress * 48.0))
    );
    float particle_side = opening > 0.5
        ? 1.0 - smoothstep(
            0.0,
            edge_width * 3.5,
            max(signed_distance, 0.0)
        )
        : 1.0 - smoothstep(
            0.0,
            edge_width * 3.5,
            max(-signed_distance, 0.0)
        );
    float particle = step(0.978, particle_seed) * frontier * particle_side;
    particle *= smoothstep(
        pixel_y,
        max(local_band_height * 0.10, pixel_y * 2.0),
        min(
            uv.y - top,
            bottom - uv.y
        )
    );

    float overlay_alpha = clamp(
        source.a * source_mask * 0.96 + glow_alpha + ignition * 0.42 +
            particle * 0.72,
        0.0,
        1.0
    );
    vec3 overlay_rgb = source.rgb * source_mask * 0.96;
    overlay_rgb += element * glow_alpha;
    overlay_rgb += mix(uStarlight, element, 0.45) * ignition * 0.42;
    overlay_rgb += mix(uGold, element, 0.60) * particle * 0.72;
    overlay_rgb += mix(uAstral, uVoid, 0.68) * frontier * 0.045;

    fragColor = vec4(min(overlay_rgb, vec3(overlay_alpha)), overlay_alpha);
}
