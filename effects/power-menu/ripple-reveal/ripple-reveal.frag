// Power-menu ripple reveal.
// A mana ignition begins at the taskbar power button and tears a warped,
// transparent boundary toward the opposite corner. The power-menu scene only
// exists behind that advancing front; the live desktop remains visible ahead
// of it because the unrevealed pixels have zero alpha.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;
in vec2 v_texcoord;

uniform float progress;
uniform vec2 resolution;
uniform sampler2D tex;
uniform vec2 origin;
uniform float opening;
uniform vec3 uGold;
uniform vec3 uStarlight;
uniform vec3 uAstral;

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
        mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0)), u.x),
        u.y
    );
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.56;
    for (int octave = 0; octave < 4; ++octave) {
        value += noise2(p) * amplitude;
        p = p * 2.07 + vec2(13.7, 7.9);
        amplitude *= 0.48;
    }
    return value;
}

float smoother(float value) {
    value = clamp(value, 0.0, 1.0);
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
}

vec2 metric_from_origin(vec2 uv, float aspect) {
    return (uv - origin) * vec2(aspect, 1.0);
}

float maximum_corner_distance(float aspect) {
    float distance_value = 0.0;
    distance_value = max(distance_value, length(metric_from_origin(vec2(0.0, 0.0), aspect)));
    distance_value = max(distance_value, length(metric_from_origin(vec2(1.0, 0.0), aspect)));
    distance_value = max(distance_value, length(metric_from_origin(vec2(0.0, 1.0), aspect)));
    distance_value = max(distance_value, length(metric_from_origin(vec2(1.0, 1.0), aspect)));
    return distance_value;
}

void main() {
    vec2 uv = v_texcoord;
    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2 metric = metric_from_origin(uv, aspect);
    float distance_from_origin = length(metric);
    float maximum_distance = maximum_corner_distance(aspect);

    vec2 target_metric = metric_from_origin(vec2(1.0, 0.0), aspect);
    vec2 travel_dir = normalize(target_metric);
    vec2 cross_dir = vec2(-travel_dir.y, travel_dir.x);
    float forward = dot(metric, travel_dir);
    float lateral = dot(metric, cross_dir);
    float travel_extent = max(length(target_metric), 0.0001);
    float forward01 = clamp(forward / travel_extent, 0.0, 1.0);

    // Keep the terminal visible frame pixel-exact so the handoff back to the
    // live video cannot produce a one-frame shimmer.
    if (progress >= 0.9995) {
        fragColor = texture(tex, uv);
        return;
    }
    if (progress <= 0.0001) {
        fragColor = vec4(0.0);
        return;
    }

    // Hold briefly for the button ignition, then spend almost the entire
    // timeline carrying the rupture across the display. The previous mapping
    // compressed the full sweep into progress 0.055..0.210, which made duration
    // changes nearly invisible because the actual travel still lasted ~200 ms.
    float launch_progress = clamp((progress - 0.075) / 0.865, 0.0, 1.0);
    float launch = smoother(launch_progress);
    float radius = mix(-0.055, maximum_distance + 0.105, launch);

    // Bias the wave to travel more aggressively along the bottom-left to
    // top-right diagonal instead of reading as an evenly expanding circle.
    float directional_distance = length(vec2(
        mix(forward, max(forward, 0.0) * 0.76 + min(forward, 0.0) * 1.15, 1.0),
        lateral * 1.22
    ));
    float base_distance = mix(distance_from_origin, directional_distance, 0.74);

    float angle = atan(metric.y, metric.x);
    float broad_noise = fbm(vec2(forward * 1.65, lateral * 1.35) +
        vec2(progress * 2.2, -progress * 1.3)) - 0.5;
    float fracture = sin(lateral * 11.0 + forward * 4.7 + broad_noise * 5.2 + progress * 4.8);
    float barbs = sin(angle * 8.5 - broad_noise * 6.1 + progress * 4.2);
    float fine_noise = noise2(uv * resolution / 46.0 + vec2(progress * 4.0, -progress * 2.2)) - 0.5;
    float spur_window = smoothstep(0.06, 0.86, forward01) * (1.0 - smoothstep(0.88, 1.0, forward01));

    // Large-scale noise sets the main silhouette; the mixed sine terms provide
    // hand-drawn fracture points so the front feels like a magical rupture,
    // not a perfect ellipse.
    float distortion =
        broad_noise * 0.070 +
        fracture * 0.018 +
        barbs * 0.014 +
        fine_noise * 0.008 +
        spur_window * (0.021 + broad_noise * 0.010);

    float field = base_distance + distortion;
    float signed_front = field - radius;

    float feather = mix(0.017, 0.010, smoothstep(0.12, 0.78, progress));
    float reveal = 1.0 - smoothstep(-feather, feather, signed_front);

    float prewave_pull = smoothstep(0.105, 0.018, signed_front) * step(0.0, signed_front);
    float turbulence = 1.0 - smoothstep(0.010, 0.085, abs(signed_front));
    vec2 radial_direction = distance_from_origin > 0.0001
        ? normalize(metric) / vec2(aspect, 1.0)
        : vec2(0.0);
    vec2 tangent_direction = vec2(-radial_direction.y, radial_direction.x);
    vec2 travel_direction_uv = normalize(vec2(travel_dir.x / aspect, travel_dir.y));
    float direction_sign = mix(-1.0, 1.0, opening);

    vec2 sample_offset = (
        radial_direction * broad_noise * 0.010 +
        tangent_direction * fracture * 0.0055 -
        travel_direction_uv * prewave_pull * 0.012 * direction_sign
    ) * (turbulence * 0.85 + prewave_pull * 0.55);
    vec4 scene = texture(tex, clamp(uv + sample_offset, vec2(0.0), vec2(1.0)));

    float thin_core = 1.0 - smoothstep(0.0025, 0.012, abs(signed_front));
    float turbulent_band = 1.0 - smoothstep(0.010, 0.060, abs(signed_front));
    float outer_halo = 1.0 - smoothstep(0.030, 0.125, abs(signed_front));
    float ahead_haze = smoothstep(0.090, 0.016, signed_front) * step(0.0, signed_front);
    float endpoint_window = smoothstep(0.02, 0.08, progress) *
        (1.0 - smoothstep(0.955, 0.998, progress));

    // The front evolves from Arthur-side mana gold and starlight into Sylvie-
    // side Aether violet as it travels toward the top-right corner.
    float diagonal_travel = clamp(forward01, 0.0, 1.0);
    vec3 mana_colour = mix(uGold, uStarlight, smoothstep(0.02, 0.45, diagonal_travel));
    vec3 edge_colour = mix(mana_colour, uAstral, smoothstep(0.45, 0.95, diagonal_travel));
    vec3 core_colour = mix(edge_colour, vec3(1.0), 0.26);

    // A more dramatic ignition pulse sits directly over the real taskbar
    // button: bright core first, then a thin expanding ring that hands off to
    // the travelling front.
    float ignition_window = smoothstep(0.004, 0.020, progress) *
        (1.0 - smoothstep(0.19, 0.33, progress));
    float ignition_radius = mix(0.004, 0.080, smoother(min(progress / 0.24, 1.0)));
    float ignition_ring = exp(-pow((distance_from_origin - ignition_radius) / 0.0085, 2.0));
    float ignition_core = exp(-distance_from_origin * distance_from_origin / 0.00078) *
        (1.0 - smoothstep(0.018, 0.11, progress));
    float ignition_alpha = (ignition_ring * 0.92 + ignition_core * 0.68) * ignition_window;

    // Give the ripple a more graceful finish near the destination corner by
    // letting a faint astral residue linger for a brief moment before settling.
    float destination_distance = length(metric - target_metric);
    float arrival_window = smoothstep(0.84, 0.96, progress) *
        (1.0 - smoothstep(0.985, 1.0, progress));
    float arrival_residue = exp(-pow(destination_distance / 0.14, 2.0)) * arrival_window;

    float core_alpha = thin_core * 0.84 * endpoint_window;
    float band_alpha = turbulent_band * 0.31 * endpoint_window;
    float halo_alpha = outer_halo * 0.13 * endpoint_window;
    float haze_alpha = ahead_haze * 0.11 * endpoint_window;
    float glow_alpha = clamp(
        core_alpha + band_alpha + halo_alpha + haze_alpha + ignition_alpha + arrival_residue * 0.28,
        0.0,
        0.98
    );

    vec3 glow_premult =
        core_colour * core_alpha +
        edge_colour * (band_alpha + halo_alpha) +
        uStarlight * haze_alpha +
        mix(uGold, uStarlight, 0.38) * ignition_alpha +
        mix(uStarlight, uAstral, 0.78) * (arrival_residue * 0.28);

    // The downloaded GTK frame is premultiplied. Preserve that convention for
    // both the scene and procedural glow so GtkGLArea composites cleanly over
    // the still-live desktop beneath this transparent layer surface.
    float scene_alpha = scene.a * reveal;
    vec3 scene_rgb = scene.rgb * reveal;
    float output_alpha = scene_alpha + glow_alpha * (1.0 - scene_alpha);
    vec3 output_rgb = scene_rgb + glow_premult * (1.0 - scene_alpha);

    fragColor = vec4(min(output_rgb, vec3(1.45)), output_alpha);
}
