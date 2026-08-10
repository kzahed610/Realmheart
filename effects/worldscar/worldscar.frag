#version 300 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D previousTex;
uniform sampler2D candidateTex;
uniform sampler2D nextTex;
uniform sampler2D previousFarTex;
uniform sampler2D nextFarTex;
uniform sampler2D previousFarFarTex;
uniform vec2 resolution;
uniform float openProgress;
uniform float commitProgress;
uniform float finishProgress;
uniform float navigationProgress;
uniform float navigationDirection;
uniform float timeSeconds;
uniform float previousReady;
uniform float nextReady;
uniform float previousFarReady;
uniform float nextFarReady;
uniform float previousFarFarReady;
uniform vec4 previousUv;
uniform vec4 candidateUv;
uniform vec4 nextUv;
uniform vec4 previousFarUv;
uniform vec4 nextFarUv;
uniform vec4 previousFarFarUv;
// Destination crop rectangles used while a wallpaper moves between authored
// chamber roles. Interpolating these prevents the final role rotation from
// snapping the image framing even when the cavity geometry itself is smooth.
uniform vec4 previousAsSelectedUv;
uniform vec4 selectedAsPreviousUv;
uniform vec4 selectedAsNextUv;
uniform vec4 nextAsSelectedUv;
// previousFarFarTex is a true resident look-behind slot. During Up it enters
// the vacated top chamber while the newly exposed farther neighbour decodes
// into the hidden slot that falls off the opposite end of the six-role ring.

// The GTK overlay intentionally owns only the left 62% of the monitor. Shader
// authoring coordinates remain screen-normalized so the wound keeps the same
// physical composition while the transparent right 38% allocates no GL buffer.
const float WORLDSCAR_SURFACE_FRACTION = 0.62;

float local_x(float screen_x) {
    return screen_x / WORLDSCAR_SURFACE_FRACTION;
}

vec2 screen_uv(vec2 local_uv) {
    return vec2(local_uv.x * WORLDSCAR_SURFACE_FRACTION, local_uv.y);
}

vec2 crop_uv(vec4 rect, vec2 uv) {
    return mix(rect.xy, rect.zw, uv);
}

vec2 preview_local_uv(vec2 uv, vec4 bounds) {
    vec2 size = max(bounds.zw - bounds.xy, vec2(0.0001));
    return clamp((uv - bounds.xy) / size, 0.0, 1.0);
}

vec4 previous_far_preview_bounds() {
    return vec4(local_x(0.392), 0.000, local_x(0.606), 0.238);
}

vec4 previous_preview_bounds() {
    return vec4(local_x(0.309), 0.064, local_x(0.501), 0.339);
}

vec4 selected_preview_bounds() {
    return vec4(local_x(0.080), 0.220, local_x(0.520), 0.899);
}

vec4 next_preview_bounds() {
    return vec4(local_x(0.000), 0.770, local_x(0.286), 0.999);
}

float saturate(float value) {
    return clamp(value, 0.0, 1.0);
}

float ease_out_cubic(float value) {
    float inverse = 1.0 - saturate(value);
    return 1.0 - inverse * inverse * inverse;
}

float smooth_min(float a, float b, float radius) {
    float h = saturate(0.5 + 0.5 * (b - a) / max(radius, 0.0001));
    return mix(b, a, h) - radius * h * (1.0 - h);
}

vec2 scar_point(vec2 uv_point, float aspect) {
    return vec2(local_x(uv_point.x) * aspect, uv_point.y);
}

// Elliptical SDF authored in screen-normalized coordinates. The scalar return
// value remains in the same approximate distance domain as tapered_segment(),
// so it can be unioned and carved without introducing a second geometry path.
float scar_ellipse_field(
    vec2 point,
    vec2 center_uv,
    vec2 radius_uv,
    float aspect
) {
    vec2 center = scar_point(center_uv, aspect);
    vec2 radius = vec2(local_x(radius_uv.x) * aspect, radius_uv.y);
    vec2 normalized = (point - center) / max(radius, vec2(0.0001));
    return (length(normalized) - 1.0) * min(radius.x, radius.y);
}

// Boolean subtraction for authored concave tears. These edge bites are what
// keep the three preview chambers reading as one fractured scar rather than
// three soft portal blobs.
float carve_notch(float field, float notch_field) {
    return max(field, -notch_field);
}

float tapered_segment(
    vec2 point,
    vec2 start,
    vec2 end,
    float start_radius,
    float end_radius
) {
    vec2 axis = end - start;
    float axis_length_sq = max(dot(axis, axis), 0.000001);
    float along = saturate(dot(point - start, axis) / axis_length_sq);
    vec2 nearest = start + axis * along;
    float radius = mix(start_radius, end_radius, along);
    return length(point - nearest) - radius;
}

float line_segment_distance(vec2 point, vec2 start, vec2 end) {
    vec2 axis = end - start;
    float axis_length_sq = max(dot(axis, axis), 0.000001);
    float along = saturate(dot(point - start, axis) / axis_length_sq);
    return length(point - (start + axis * along));
}

float cross_2d(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// Signed distance to a convex quadrilateral. This is opening-only geometry:
// unlike tapered_segment(), its ends are literal cuts rather than circles.
float convex_quad_field(
    vec2 point,
    vec2 a,
    vec2 b,
    vec2 c,
    vec2 d
) {
    float distance_to_edge = min(
        min(line_segment_distance(point, a, b),
            line_segment_distance(point, b, c)),
        min(line_segment_distance(point, c, d),
            line_segment_distance(point, d, a))
    );

    float c0 = cross_2d(b - a, point - a);
    float c1 = cross_2d(c - b, point - b);
    float c2 = cross_2d(d - c, point - c);
    float c3 = cross_2d(a - d, point - d);
    bool all_positive = c0 >= 0.0 && c1 >= 0.0 &&
        c2 >= 0.0 && c3 >= 0.0;
    bool all_negative = c0 <= 0.0 && c1 <= 0.0 &&
        c2 <= 0.0 && c3 <= 0.0;
    return (all_positive || all_negative)
        ? -distance_to_edge
        : distance_to_edge;
}

// A tapered blade stroke with independently slanted start/end cuts. Positive
// slant leans the +normal side forward along the stroke and pulls the opposite
// side backward, producing a deliberate bevel instead of a capsule cap.
float blade_segment(
    vec2 point,
    vec2 start,
    vec2 end,
    float start_width,
    float end_width,
    float start_slant,
    float end_slant
) {
    vec2 axis = end - start;
    float axis_length = max(length(axis), 0.0001);
    vec2 direction = axis / axis_length;
    vec2 normal = vec2(-direction.y, direction.x);

    vec2 a = start + normal * start_width + direction * start_slant;
    vec2 b = end + normal * end_width + direction * end_slant;
    vec2 c = end - normal * end_width - direction * end_slant;
    vec2 d = start - normal * start_width - direction * start_slant;
    return convex_quad_field(point, a, b, c, d);
}

// Convenience wrapper for polygonal wound plates authored directly in
// screen-normalized scar coordinates. Pass 14 uses these hard-edged plates as
// the dominant open-silhouette language instead of exposing ellipse contours.
float scar_quad_field(
    vec2 point,
    vec2 a_uv,
    vec2 b_uv,
    vec2 c_uv,
    vec2 d_uv,
    float aspect
) {
    return convex_quad_field(
        point,
        scar_point(a_uv, aspect),
        scar_point(b_uv, aspect),
        scar_point(c_uv, aspect),
        scar_point(d_uv, aspect)
    );
}

float contour_damage(vec2 point, float phase) {
    return (
        0.0025 * sin(point.y * 34.0 + point.x * 7.0 + phase) +
        0.0011 * sin(point.y * 67.0 - point.x * 13.0 - phase * 0.8)
    );
}

float aether_turbulence(vec2 point, float phase) {
    // Low-frequency authored turbulence for FX only. This is intentionally not
    // geometry noise; it simply keeps the aether layers from reading as flat
    // uniform bands once the silhouette has been locked.
    float flow = 0.50;
    flow += 0.22 * sin(point.y * 18.0 - point.x * 4.8 + phase * 1.7);
    flow += 0.16 * sin(point.y * 39.0 + point.x * 9.0 - phase * 2.1);
    flow += 0.12 * sin(point.y * 73.0 - point.x * 15.0 + phase * 0.8);
    return saturate(flow);
}

float wrapped_distance(float a, float b) {
    float distance = abs(fract(a) - fract(b));
    return min(distance, 1.0 - distance);
}

float travelling_aether_pulse(float along, float time_seconds) {
    // Two different-speed packets stop the border from feeling like a single
    // loading-spinner highlight. They are deliberately broad enough to read
    // clearly at 1080p without turning the whole wound white.
    float first = 1.0 - smoothstep(
        0.018,
        0.105,
        wrapped_distance(along, time_seconds * 0.115)
    );
    float second = 1.0 - smoothstep(
        0.012,
        0.072,
        wrapped_distance(along, 0.58 - time_seconds * 0.072)
    );
    return saturate(max(first, second * 0.68));
}


// Pass 44: borrow the *layering philosophy* from Realmheart Void rather than
// its circular geometry. Coherent noise gives the Worldscar's FX spatial
// structure: billowing aether, hot junctions and sparse branching fractures.
float worldscar_hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float worldscar_hash11(float p) {
    return fract(sin(p * 127.1 + 31.7) * 43758.5453123);
}

float worldscar_noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(
            worldscar_hash12(i),
            worldscar_hash12(i + vec2(1.0, 0.0)),
            u.x
        ),
        mix(
            worldscar_hash12(i + vec2(0.0, 1.0)),
            worldscar_hash12(i + vec2(1.0, 1.0)),
            u.x
        ),
        u.y
    );
}

float worldscar_fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.55;
    for (int octave = 0; octave < 3; ++octave) {
        value += worldscar_noise2(p) * amplitude;
        p = p * 2.13 + 17.0;
        amplitude *= 0.5;
    }
    return value;
}

vec2 worldscar_domain_warp(vec2 point, float time_seconds) {
    // Two decorrelated low-frequency channels bend the FX sampling domain in
    // both axes. The maximum displacement stays below one percent of normalized
    // screen height, so the locked cavity masks never inherit this damage.
    vec2 drift = vec2(time_seconds * 0.012, -time_seconds * 0.009);
    float warp_x = worldscar_fbm(
        point * vec2(8.5, 6.5) + drift + vec2(7.3, 19.1)
    );
    float warp_y = worldscar_fbm(
        point.yx * vec2(7.2, 9.4) - drift.yx + vec2(31.7, 11.3)
    );
    return (vec2(warp_x, warp_y) - vec2(0.48)) * vec2(0.016, 0.013);
}

vec3 content_reactive_rim(vec3 content) {
    // The exterior stays Aether-violet while the inner ridge borrows a bounded
    // amount of warmth and luminance from the world revealed beneath it.
    float luma = dot(content, vec3(0.2126, 0.7152, 0.0722));
    float warmth = smoothstep(
        -0.08,
        0.32,
        content.r - content.b * 0.75 + (content.r - content.g) * 0.15
    );
    vec3 violet_response = vec3(0.66, 0.30, 1.00);
    vec3 warm_response = vec3(1.00, 0.42, 0.20);
    vec3 response = mix(violet_response, warm_response, warmth * 0.54);
    return mix(
        response,
        vec3(0.985, 0.940, 1.00),
        smoothstep(0.68, 1.00, luma) * 0.28
    );
}

float worldscar_ember_field(
    vec2 point,
    float outside_distance,
    float time_seconds
) {
    // Sparse one-pixel motes drift upward through a narrow exterior band. Cells
    // are stable in screen space; only their tiny local centers move, avoiding
    // the crawling full-screen grain that would flatten the wound again.
    if (outside_distance <= 0.006 || outside_distance >= 0.070) return 0.0;

    vec2 grid = vec2(38.0, 32.0);
    vec2 cell = floor(point * grid);
    vec2 local = fract(point * grid) - 0.5;
    float seed = worldscar_hash12(cell + vec2(13.2, 7.9));
    float enabled = smoothstep(0.940, 0.985, seed);
    float phase = fract(time_seconds * 0.055 + seed);
    vec2 center = vec2(
        (worldscar_hash12(cell + vec2(2.7, 19.4)) - 0.5) * 0.70,
        mix(0.34, -0.34, phase) +
            (worldscar_hash12(cell + vec2(29.1, 4.6)) - 0.5) * 0.12
    );
    float mote_distance = length(local - center);
    float core = 1.0 - smoothstep(0.018, 0.050, mote_distance);
    float aura = 1.0 - smoothstep(0.045, 0.140, mote_distance);
    float lifecycle = smoothstep(0.0, 0.14, phase) *
        (1.0 - smoothstep(0.76, 1.0, phase));
    float edge_fade = smoothstep(0.006, 0.014, outside_distance) *
        (1.0 - smoothstep(0.046, 0.070, outside_distance));
    return enabled * lifecycle * edge_fade * (core + aura * 0.22);
}

float procedural_edge_fractures(
    float along,
    float outside_distance,
    vec2 point,
    float time_seconds
) {
    // Work in edge-phase/distance space so branches grow *away* from the
    // existing locked silhouette instead of inventing new cavity geometry.
    const float reach = 0.095;
    if (outside_distance <= 0.00001 || outside_distance >= reach) {
        return 0.0;
    }
    float distance01 = saturate(outside_distance / reach);
    float scaled = along * 11.0;
    float cell = floor(scaled);
    float local = fract(scaled) - 0.5;

    // Only some edge cells sprout a branch. This prevents the old "hairy
    // outline" problem and leaves room for deliberate hot junctions.
    float seed = worldscar_hash11(cell + 4.37);
    float enabled = smoothstep(0.56, 0.72, seed);

    float slope = mix(-0.68, 0.68, worldscar_hash11(cell + 11.8));
    float bend = (
        worldscar_noise2(
            point * vec2(19.0, 15.0) +
            vec2(cell * 0.17, time_seconds * 0.055)
        ) - 0.50
    ) * 0.060;
    float curve = mix(-0.34, 0.34, worldscar_hash11(cell + 31.7));
    float wiggle = sin(
        distance01 * 5.6 + seed * 12.0 + time_seconds * 0.12
    ) * 0.052 * distance01;
    float primary_center =
        slope * distance01 * 0.25 +
        curve * distance01 * distance01 +
        bend * distance01 + wiggle;
    float primary_width = mix(
        0.060,
        0.014,
        smoothstep(0.0, 1.0, distance01)
    );
    float primary = 1.0 - smoothstep(
        primary_width * 0.30,
        primary_width,
        abs(local - primary_center)
    );

    // Split some branches after they have left the rim. The second arm is
    // shorter/dimmer, giving the target-reference forked lightning language.
    float split_slope = slope + mix(
        -0.45,
        0.45,
        worldscar_hash11(cell + 23.1)
    );
    float split_age = saturate((distance01 - 0.30) / 0.70);
    float split_curve = mix(
        -0.24,
        0.24,
        worldscar_hash11(cell + 47.3)
    );
    float split_center =
        primary_center +
        split_slope * max(distance01 - 0.30, 0.0) * 0.24 +
        split_curve * split_age * split_age;
    float split_width = mix(0.044, 0.010, split_age);
    float split = 1.0 - smoothstep(
        split_width * 0.28,
        split_width,
        abs(local - split_center)
    );
    split *= smoothstep(0.27, 0.42, distance01);

    float reach_fade = 1.0 - smoothstep(0.70, 1.0, distance01);
    float flicker = 0.84 + 0.16 * sin(time_seconds * 2.7 + cell * 2.13);
    return enabled * max(primary, split * 0.82) * reach_fade * flicker;
}

// Three readable realities ride ONE broad upper-right -> lower-left wound.
// Pass 07 changes ONLY this authored silhouette. The loading/cache pipeline,
// continuous opening frontier, navigation morph and Enter handoff stay exactly
// as they were in Pass 06.
//
// The shape language follows the concept art more closely now: three large
// reality chambers along one diagonal spine, a dominant selected chamber in the
// middle, narrow necks between them, and deliberately concave torn bites. The
// result should read as one damaged slab of reality rather than a crescent or
// three independent rounded portals.

vec2 screen_uv_from_point(vec2 point, float aspect) {
    return vec2(
        point.x * WORLDSCAR_SURFACE_FRACTION / max(aspect, 0.0001),
        point.y
    );
}

float scar_progress_from_point(vec2 point, float aspect) {
    vec2 uv = screen_uv_from_point(point, aspect);
    vec2 start = vec2(0.455, 0.055);
    vec2 end = vec2(0.070, 0.955);
    vec2 axis = end - start;
    return saturate(dot(uv - start, axis) / max(dot(axis, axis), 0.000001));
}

float progress_range_field(float along, float start, float end, float scale) {
    float lower = (start - along) * scale;
    float upper = (along - end) * scale;
    return max(lower, upper);
}

float fissure_divider(vec2 point, float aspect, vec2 a, vec2 b) {
    vec2 uv = screen_uv_from_point(point, aspect);
    vec2 axis = b - a;
    return cross_2d(axis, uv - a) / max(length(axis), 0.0001);
}

float sample_curve(
    float y,
    float y0, float x0,
    float y1, float x1,
    float y2, float x2,
    float y3, float x3,
    float y4, float x4,
    float y5, float x5,
    float y6, float x6,
    float y7, float x7,
    float y8, float x8,
    float y9, float x9,
    float y10, float x10
) {
    if (y <= y0) return x0;
    if (y < y1) return mix(x0, x1, smoothstep(y0, y1, y));
    if (y < y2) return mix(x1, x2, smoothstep(y1, y2, y));
    if (y < y3) return mix(x2, x3, smoothstep(y2, y3, y));
    if (y < y4) return mix(x3, x4, smoothstep(y3, y4, y));
    if (y < y5) return mix(x4, x5, smoothstep(y4, y5, y));
    if (y < y6) return mix(x5, x6, smoothstep(y5, y6, y));
    if (y < y7) return mix(x6, x7, smoothstep(y6, y7, y));
    if (y < y8) return mix(x7, x8, smoothstep(y7, y8, y));
    if (y < y9) return mix(x8, x9, smoothstep(y8, y9, y));
    if (y < y10) return mix(x9, x10, smoothstep(y9, y10, y));
    return x10;
}

float upper_left_curve(float y) {
    return sample_curve(
        y,
        0.064, 0.365,
        0.106, 0.335,
        0.149, 0.339,
        0.191, 0.347,
        0.213, 0.352,
        0.234, 0.352,
        0.276, 0.339,
        0.319, 0.328,
        0.339, 0.309,
        0.339, 0.309,
        0.339, 0.309
    );
}

float upper_right_curve(float y) {
    return sample_curve(
        y,
        0.064, 0.405,
        0.106, 0.413,
        0.149, 0.432,
        0.191, 0.450,
        0.213, 0.462,
        0.234, 0.490,
        0.276, 0.489,
        0.319, 0.491,
        0.339, 0.501,
        0.339, 0.501,
        0.339, 0.501
    );
}

float top_left_curve(float y) {
    return sample_curve(
        y,
        0.000, 0.393,
        0.021, 0.400,
        0.064, 0.405,
        0.106, 0.413,
        0.149, 0.432,
        0.191, 0.450,
        0.213, 0.462,
        0.224, 0.474,
        0.230, 0.485,
        0.234, 0.490,
        0.238, 0.494
    );
}

float top_right_curve(float y) {
    return sample_curve(
        y,
        0.000, 0.606,
        0.021, 0.593,
        0.064, 0.568,
        0.106, 0.542,
        0.149, 0.529,
        0.191, 0.508,
        0.213, 0.506,
        0.224, 0.500,
        0.230, 0.497,
        0.234, 0.494,
        0.238, 0.494
    );
}

float main_left_curve(float y) {
    return sample_curve(
        y,
        0.339, 0.309,
        0.405, 0.302,
        0.471, 0.292,
        0.537, 0.270,
        0.603, 0.238,
        0.668, 0.198,
        0.734, 0.166,
        0.800, 0.138,
        0.833, 0.126,
        0.866, 0.138,
        0.899, 0.282
    );
}

float main_right_curve(float y) {
    return sample_curve(
        y,
        0.339, 0.501,
        0.405, 0.506,
        0.471, 0.500,
        0.537, 0.477,
        0.603, 0.451,
        0.668, 0.433,
        0.734, 0.416,
        0.800, 0.394,
        0.833, 0.370,
        0.866, 0.340,
        0.899, 0.300
    );
}

float main_crown_top_y(float x) {
    // Same Pass 32 crown vertices, now represented as ONE continuous
    // piecewise-linear boundary. The old implementation built the crown from
    // touching trapezoid strips; their shared vertical sides had field == 0,
    // so the edge FX faithfully drew those internal seams as tiny hanging
    // lines/squares over the wallpaper.
    const float x0  = 0.270;
    const float x1  = 0.278;
    const float x2  = 0.292;
    const float x3  = 0.315;
    const float x4  = 0.346;
    const float x5  = 0.368;
    const float x6  = 0.382;
    const float x7  = 0.412;
    const float x8  = 0.438;
    const float x9  = 0.454;
    const float x10 = 0.468;
    const float x11 = 0.486;
    const float x12 = 0.500;

    const float y0  = 0.425;
    const float y1  = 0.360;
    const float y2  = 0.325;
    const float y3  = 0.292;
    const float y4  = 0.266;
    const float y5  = 0.255;
    const float y6  = 0.262;
    const float y7  = 0.266;
    const float y8  = 0.285;
    const float y9  = 0.313;
    const float y10 = 0.323;
    const float y11 = 0.319;
    const float y12 = 0.334;

    if (x <= x0) return y0;
    if (x < x1)  return mix(y0, y1, (x - x0) / (x1 - x0));
    if (x < x2)  return mix(y1, y2, (x - x1) / (x2 - x1));
    if (x < x3)  return mix(y2, y3, (x - x2) / (x3 - x2));
    if (x < x4)  return mix(y3, y4, (x - x3) / (x4 - x3));
    if (x < x5)  return mix(y4, y5, (x - x4) / (x5 - x4));
    if (x < x6)  return mix(y5, y6, (x - x5) / (x6 - x5));
    if (x < x7)  return mix(y6, y7, (x - x6) / (x7 - x6));
    if (x < x8)  return mix(y7, y8, (x - x7) / (x8 - x7));
    if (x < x9)  return mix(y8, y9, (x - x8) / (x9 - x8));
    if (x < x10) return mix(y9, y10, (x - x9) / (x10 - x9));
    if (x < x11) return mix(y10, y11, (x - x10) / (x11 - x10));
    if (x < x12) return mix(y11, y12, (x - x11) / (x12 - x11));
    return y12;
}

float main_crown_field(vec2 point, float aspect) {
    vec2 uv = screen_uv_from_point(point, aspect);
    const float left_x = 0.270;
    const float right_x = 0.500;
    const float base_y = 0.440;

    float side = max(left_x - uv.x, uv.x - right_x);
    float top = main_crown_top_y(uv.x) - uv.y;
    float bottom = uv.y - base_y;
    return max(side, max(top, bottom));
}


float bottom_crest_top_y(float x) {
    // Piecewise-linear top boundary. Keeping this as one continuous boundary
    // avoids the internal quad seams that made Pass 35's crest look segmented.
    const float x0 = 0.045;
    const float x1 = 0.066;
    const float x2 = 0.090;
    const float x3 = 0.113;
    const float x4 = 0.136;
    const float x5 = 0.159;
    const float x6 = 0.184;
    const float x7 = 0.214;

    const float y0 = 0.878;
    const float y1 = 0.850;
    const float y2 = 0.812;
    const float y3 = 0.780;
    const float y4 = 0.768;
    const float y5 = 0.790;
    const float y6 = 0.826;
    const float y7 = 0.858;

    if (x <= x0) return y0;
    if (x < x1) return mix(y0, y1, (x - x0) / (x1 - x0));
    if (x < x2) return mix(y1, y2, (x - x1) / (x2 - x1));
    if (x < x3) return mix(y2, y3, (x - x2) / (x3 - x2));
    if (x < x4) return mix(y3, y4, (x - x3) / (x4 - x3));
    if (x < x5) return mix(y4, y5, (x - x4) / (x5 - x4));
    if (x < x6) return mix(y5, y6, (x - x5) / (x6 - x5));
    if (x < x7) return mix(y6, y7, (x - x6) / (x7 - x6));
    return y7;
}

float bottom_crest_field(vec2 point, float aspect) {
    vec2 uv = screen_uv_from_point(point, aspect);
    const float left_x = 0.045;
    const float right_x = 0.214;
    const float base_y = 0.892;

    float side = max(left_x - uv.x, uv.x - right_x);
    float top = bottom_crest_top_y(uv.x) - uv.y;
    float bottom = uv.y - base_y;
    return max(side, max(top, bottom));
}

float bottom_left_curve(float y) {
    return sample_curve(
        y,
        0.816, 0.110,
        0.836, 0.089,
        0.856, 0.068,
        0.878, 0.050,
        0.900, 0.040,
        0.924, 0.036,
        0.948, 0.040,
        0.972, 0.048,
        0.986, 0.055,
        0.995, 0.060,
        0.999, 0.066
    );
}

float bottom_right_curve(float y) {
    return sample_curve(
        y,
        0.816, 0.180,
        0.836, 0.205,
        0.856, 0.229,
        0.878, 0.252,
        0.900, 0.266,
        0.924, 0.274,
        0.948, 0.272,
        0.972, 0.252,
        0.986, 0.228,
        0.995, 0.205,
        0.999, 0.188
    );
}

float region_from_curves(
    vec2 point,
    float aspect,
    float y0,
    float y1,
    float left_x,
    float right_x
) {
    vec2 uv = screen_uv_from_point(point, aspect);
    float side = max(left_x - uv.x, uv.x - right_x);
    float vertical = max(y0 - uv.y, uv.y - y1);
    return max(side, vertical);
}

float top_world_field(vec2 point, float aspect, float opening) {
    vec2 uv = screen_uv_from_point(point, aspect);
    float field = region_from_curves(
        point,
        aspect,
        0.000,
        0.238,
        top_left_curve(uv.y),
        top_right_curve(uv.y)
    );
    return field;
}

float previous_world_field(vec2 point, float aspect, float opening) {
    vec2 uv = screen_uv_from_point(point, aspect);
    float field = region_from_curves(
        point,
        aspect,
        0.064,
        0.339,
        upper_left_curve(uv.y),
        upper_right_curve(uv.y)
    );
    return field;
}

float selected_world_field(vec2 point, float aspect, float opening) {
    vec2 uv = screen_uv_from_point(point, aspect);
    float field = region_from_curves(
        point,
        aspect,
        0.339,
        0.899,
        main_left_curve(uv.y),
        main_right_curve(uv.y)
    );

    float crown = main_crown_field(point, aspect);
    field = min(field, crown);
    return field;
}

float next_world_field(vec2 point, float aspect, float opening) {
    vec2 uv = screen_uv_from_point(point, aspect);
    float field = region_from_curves(
        point,
        aspect,
        0.848,
        0.999,
        bottom_left_curve(uv.y),
        bottom_right_curve(uv.y)
    );

    float crest = bottom_crest_field(point, aspect);
    field = min(field, crest);

    // Keep smaller secondary shaping unions so the pocket remains organic, but
    // avoid the large capsule behavior from earlier passes.
    float belly_rounding = scar_ellipse_field(
        point,
        vec2(0.134, 0.930),
        vec2(0.098, 0.060),
        aspect
    );
    float left_tail_rounding = scar_ellipse_field(
        point,
        vec2(0.068, 0.960),
        vec2(0.052, 0.028),
        aspect
    );
    float underside_rounding = scar_ellipse_field(
        point,
        vec2(0.140, 0.983),
        vec2(0.084, 0.024),
        aspect
    );
    field = smooth_min(field, belly_rounding, 0.006);
    field = smooth_min(field, left_tail_rounding, 0.004);
    field = smooth_min(field, underside_rounding, 0.004);
    return field;
}

float fracture_segment(vec2 point, vec2 start, vec2 end, float width) {
    return tapered_segment(point, start, end, width, width * 0.12);
}

// Pass 45: explicit fracture topology around the LOCKED cavity silhouette.
// The target reference is not merely a noisy rim: it has readable veins that
// leave the wound, fork, and terminate in bright stress junctions. These fields
// affect FX only; they never participate in any wallpaper mask.
float authored_fracture_web_field(vec2 point, float aspect) {
    // Pass 48: local lightning clusters anchored directly to the accepted scar.
    // Keep them short, angular and forked; they should read as stress fractures
    // escaping the wound, not giant decorative laser strokes across the desktop.
    float web = fracture_segment(
        point,
        scar_point(vec2(0.390, 0.120), aspect),
        scar_point(vec2(0.366, 0.102), aspect),
        0.00130
    );
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.366, 0.102), aspect),
        scar_point(vec2(0.344, 0.080), aspect),
        0.00102
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.366, 0.102), aspect),
        scar_point(vec2(0.342, 0.113), aspect),
        0.00072
    ));

    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.310, 0.330), aspect),
        scar_point(vec2(0.286, 0.316), aspect),
        0.00135
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.286, 0.316), aspect),
        scar_point(vec2(0.260, 0.294), aspect),
        0.00100
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.286, 0.316), aspect),
        scar_point(vec2(0.265, 0.329), aspect),
        0.00070
    ));

    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.500, 0.334), aspect),
        scar_point(vec2(0.525, 0.318), aspect),
        0.00135
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.525, 0.318), aspect),
        scar_point(vec2(0.548, 0.292), aspect),
        0.00098
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.525, 0.318), aspect),
        scar_point(vec2(0.548, 0.337), aspect),
        0.00070
    ));

    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.238, 0.603), aspect),
        scar_point(vec2(0.214, 0.589), aspect),
        0.00134
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.214, 0.589), aspect),
        scar_point(vec2(0.188, 0.566), aspect),
        0.00098
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.214, 0.589), aspect),
        scar_point(vec2(0.191, 0.601), aspect),
        0.00068
    ));

    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.451, 0.603), aspect),
        scar_point(vec2(0.474, 0.587), aspect),
        0.00126
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.474, 0.587), aspect),
        scar_point(vec2(0.496, 0.560), aspect),
        0.00094
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.474, 0.587), aspect),
        scar_point(vec2(0.497, 0.596), aspect),
        0.00066
    ));

    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.136, 0.768), aspect),
        scar_point(vec2(0.112, 0.751), aspect),
        0.00130
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.112, 0.751), aspect),
        scar_point(vec2(0.086, 0.727), aspect),
        0.00096
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.112, 0.751), aspect),
        scar_point(vec2(0.090, 0.766), aspect),
        0.00068
    ));

    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.244, 0.884), aspect),
        scar_point(vec2(0.266, 0.868), aspect),
        0.00124
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.266, 0.868), aspect),
        scar_point(vec2(0.287, 0.844), aspect),
        0.00092
    ));
    web = min(web, fracture_segment(
        point,
        scar_point(vec2(0.266, 0.868), aspect),
        scar_point(vec2(0.289, 0.877), aspect),
        0.00064
    ));

    return web;
}

float authored_fracture_node_distance(vec2 point, float aspect) {
    float nodes = length(point - scar_point(vec2(0.390, 0.120), aspect));
    nodes = min(nodes, length(point - scar_point(vec2(0.310, 0.330), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.500, 0.334), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.238, 0.603), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.451, 0.603), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.136, 0.768), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.244, 0.884), aspect)));
    return nodes;
}



// Pass 46: the reference concentrates its strongest Aether damage on the
// actual wound boundaries and on the internal chamber junctions. These two
// polylines trace the accepted upper/main and main/bottom seams directly; they
// are FX-only and never change wallpaper masks or navigation geometry.
float internal_upper_seam_field(vec2 point, float aspect) {
    float seam = fracture_segment(
        point,
        scar_point(vec2(0.346, 0.266), aspect),
        scar_point(vec2(0.368, 0.255), aspect),
        0.00165
    );
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.368, 0.255), aspect),
        scar_point(vec2(0.412, 0.266), aspect),
        0.00155
    ));
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.412, 0.266), aspect),
        scar_point(vec2(0.438, 0.285), aspect),
        0.00145
    ));
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.438, 0.285), aspect),
        scar_point(vec2(0.454, 0.313), aspect),
        0.00135
    ));
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.454, 0.313), aspect),
        scar_point(vec2(0.486, 0.319), aspect),
        0.00130
    ));
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.486, 0.319), aspect),
        scar_point(vec2(0.500, 0.334), aspect),
        0.00115
    ));
    return seam;
}

float internal_lower_seam_field(vec2 point, float aspect) {
    float seam = fracture_segment(
        point,
        scar_point(vec2(0.136, 0.768), aspect),
        scar_point(vec2(0.159, 0.790), aspect),
        0.00165
    );
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.159, 0.790), aspect),
        scar_point(vec2(0.184, 0.826), aspect),
        0.00155
    ));
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.184, 0.826), aspect),
        scar_point(vec2(0.214, 0.858), aspect),
        0.00145
    ));
    seam = min(seam, fracture_segment(
        point,
        scar_point(vec2(0.214, 0.858), aspect),
        scar_point(vec2(0.244, 0.884), aspect),
        0.00120
    ));
    return seam;
}

float internal_seam_field(vec2 point, float aspect) {
    return min(
        internal_upper_seam_field(point, aspect),
        internal_lower_seam_field(point, aspect)
    );
}

float internal_seam_node_distance(vec2 point, float aspect) {
    float nodes = length(point - scar_point(vec2(0.346, 0.266), aspect));
    nodes = min(nodes, length(point - scar_point(vec2(0.500, 0.334), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.136, 0.768), aspect)));
    nodes = min(nodes, length(point - scar_point(vec2(0.244, 0.884), aspect)));
    return nodes;
}


// Pass 47: short local fracture splinters hug the accepted wound boundary.
// These are deliberately small; the long exterior branches from Pass 45 remain
// supporting accents, while these give the border itself the broken, stressed
// material language visible in the target reference.
float border_splinter_field(vec2 point, float aspect) {
    float splinter = fracture_segment(
        point,
        scar_point(vec2(0.335, 0.106), aspect),
        scar_point(vec2(0.314, 0.094), aspect),
        0.00082
    );
    splinter = min(splinter, fracture_segment(
        point,
        scar_point(vec2(0.302, 0.405), aspect),
        scar_point(vec2(0.280, 0.392), aspect),
        0.00086
    ));
    splinter = min(splinter, fracture_segment(
        point,
        scar_point(vec2(0.238, 0.603), aspect),
        scar_point(vec2(0.214, 0.590), aspect),
        0.00090
    ));
    splinter = min(splinter, fracture_segment(
        point,
        scar_point(vec2(0.451, 0.603), aspect),
        scar_point(vec2(0.474, 0.586), aspect),
        0.00082
    ));
    splinter = min(splinter, fracture_segment(
        point,
        scar_point(vec2(0.166, 0.734), aspect),
        scar_point(vec2(0.143, 0.723), aspect),
        0.00086
    ));
    splinter = min(splinter, fracture_segment(
        point,
        scar_point(vec2(0.394, 0.800), aspect),
        scar_point(vec2(0.416, 0.786), aspect),
        0.00082
    ));
    splinter = min(splinter, fracture_segment(
        point,
        scar_point(vec2(0.228, 0.900), aspect),
        scar_point(vec2(0.247, 0.887), aspect),
        0.00078
    ));
    return splinter;
}

float internal_seam_fork_field(vec2 point, float aspect) {
    // Stronger but still local forks at the chamber junctions. These are the
    // points where the reference looks most electrically stressed.
    float fork = fracture_segment(
        point,
        scar_point(vec2(0.346, 0.266), aspect),
        scar_point(vec2(0.327, 0.247), aspect),
        0.00092
    );
    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.327, 0.247), aspect),
        scar_point(vec2(0.309, 0.236), aspect),
        0.00060
    ));
    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.346, 0.266), aspect),
        scar_point(vec2(0.330, 0.287), aspect),
        0.00078
    ));

    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.500, 0.334), aspect),
        scar_point(vec2(0.519, 0.315), aspect),
        0.00090
    ));
    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.519, 0.315), aspect),
        scar_point(vec2(0.536, 0.300), aspect),
        0.00058
    ));

    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.136, 0.768), aspect),
        scar_point(vec2(0.116, 0.749), aspect),
        0.00088
    ));
    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.116, 0.749), aspect),
        scar_point(vec2(0.098, 0.737), aspect),
        0.00058
    ));

    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.244, 0.884), aspect),
        scar_point(vec2(0.265, 0.866), aspect),
        0.00086
    ));
    fork = min(fork, fracture_segment(
        point,
        scar_point(vec2(0.265, 0.866), aspect),
        scar_point(vec2(0.282, 0.850), aspect),
        0.00056
    ));
    return fork;
}

// Sparse outer-edge lightning. These branches are intentionally authored and
// directional instead of generated from a noisy outline, so the eye follows
// sharp fracture gestures rather than the raw rounded mask beneath them.
float edge_lightning_field(vec2 point, float aspect) {
    // Pass 38: keep only the one right-side gesture the user explicitly marked
    // as aesthetically useful. All other authored outer branches were circled as
    // stray clutter and are removed for now.
    float branch = fracture_segment(
        point,
        scar_point(vec2(0.505, 0.430), aspect),
        scar_point(vec2(0.548, 0.405), aspect),
        0.00175
    );
    branch = min(
        branch,
        fracture_segment(
            point,
            scar_point(vec2(0.548, 0.405), aspect),
            scar_point(vec2(0.572, 0.371), aspect),
            0.00105
        )
    );
    return branch;
}

// The Enter handoff leaves the COMPLETE diagonal damage path behind, not a
// two-pixel tail in the corner. The broad reality cavities zip closed along
// this spine from top-right -> bottom-left, then the real wallpaper renderer
// opens outward from the exact same polyline.
// Keep the authored endpoint on the low-X side: the terminal spur ENDS ON THE
// LEFT. Future composition owns the space beside that terminal edge.
float residual_slash_field(vec2 point, float aspect) {
    float slash = fracture_segment(
        point,
        scar_point(vec2(0.455, 0.055), aspect),
        scar_point(vec2(0.405, 0.205), aspect),
        0.0058
    );
    slash = min(
        slash,
        fracture_segment(
            point,
            scar_point(vec2(0.405, 0.205), aspect),
            scar_point(vec2(0.325, 0.485), aspect),
            0.0064
        )
    );
    slash = min(
        slash,
        fracture_segment(
            point,
            scar_point(vec2(0.325, 0.485), aspect),
            scar_point(vec2(0.205, 0.755), aspect),
            0.0062
        )
    );
    slash = min(
        slash,
        fracture_segment(
            point,
            scar_point(vec2(0.205, 0.755), aspect),
            scar_point(vec2(0.070, 0.955), aspect),
            0.0054
        )
    );
    return slash;
}

// Dedicated opening slash silhouette.
//
// IMPORTANT: this is intentionally separate from residual_slash_field(). The
// opening pass needs a more authored, blade-cut silhouette, but Enter/apply is
// still unfinished and currently relies on the simpler travelling residual
// damage path above. Keeping them separate lets us sculpt the opening without
// silently changing the apply choreography.
float opening_scar_field(vec2 point, float aspect) {
    // Pass 12: move the visible thickness INTO angular blade geometry instead
    // of manufacturing most of it through SDF dilation. This preserves the
    // Pass 11 silhouette while finally allowing bevelled tips and hard corners.
    float field = blade_segment(
        point,
        scar_point(vec2(0.466, 0.036), aspect),
        scar_point(vec2(0.424, 0.172), aspect),
        0.0100,
        0.0190,
        -0.0130,
        0.0090
    );
    field = smooth_min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.429, 0.158), aspect),
            scar_point(vec2(0.355, 0.368), aspect),
            0.0180,
            0.0270,
            -0.0060,
            0.0120
        ),
        0.0016
    );
    field = smooth_min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.362, 0.350), aspect),
            scar_point(vec2(0.292, 0.532), aspect),
            0.0260,
            0.0210,
            -0.0090,
            0.0100
        ),
        0.0016
    );
    field = smooth_min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.300, 0.513), aspect),
            scar_point(vec2(0.208, 0.712), aspect),
            0.0200,
            0.0230,
            -0.0080,
            0.0080
        ),
        0.0015
    );
    field = smooth_min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.216, 0.694), aspect),
            scar_point(vec2(0.105, 0.902), aspect),
            0.0220,
            0.0150,
            -0.0070,
            0.0080
        ),
        0.0014
    );
    field = smooth_min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.112, 0.887), aspect),
            scar_point(vec2(0.036, 0.971), aspect),
            0.0140,
            0.0040,
            -0.0060,
            0.0160
        ),
        0.0012
    );

    // Hard-unioned side tears: their roots may overlap the spine, but their
    // terminal edges stay visibly cut instead of being melted into round knobs.
    field = min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.452, 0.078), aspect),
            scar_point(vec2(0.497, 0.015), aspect),
            0.0120,
            0.0020,
            -0.0050,
            0.0160
        )
    );
    field = min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.414, 0.184), aspect),
            scar_point(vec2(0.479, 0.221), aspect),
            0.0130,
            0.0030,
            -0.0050,
            0.0140
        )
    );
    field = min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.306, 0.490), aspect),
            scar_point(vec2(0.184, 0.520), aspect),
            0.0140,
            0.0030,
            -0.0040,
            0.0180
        )
    );
    field = min(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.199, 0.744), aspect),
            scar_point(vec2(0.112, 0.789), aspect),
            0.0110,
            0.0025,
            -0.0040,
            0.0140
        )
    );

    // Angular bites replace Pass 11's circular notches. They introduce hard,
    // asymmetric shoulders without turning the whole stroke into lightning.
    field = carve_notch(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.350, 0.222), aspect),
            scar_point(vec2(0.324, 0.270), aspect),
            0.0190,
            0.0030,
            -0.0070,
            0.0100
        )
    );
    field = carve_notch(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.255, 0.579), aspect),
            scar_point(vec2(0.222, 0.632), aspect),
            0.0230,
            0.0035,
            0.0040,
            0.0130
        )
    );
    field = carve_notch(
        field,
        blade_segment(
            point,
            scar_point(vec2(0.166, 0.823), aspect),
            scar_point(vec2(0.139, 0.865), aspect),
            0.0170,
            0.0030,
            -0.0050,
            0.0110
        )
    );

    // Keep only a faint contour disturbance. The authored blade geometry now
    // owns the silhouette; noise should texture it, not round or wobble it.
    field += contour_damage(point, 6.2) * 0.16;
    return field;
}

float scar_progress(vec2 local_uv) {
    vec2 uv = screen_uv(local_uv);
    vec2 start = vec2(0.455, 0.055);
    vec2 end = vec2(0.070, 0.955);
    vec2 axis = end - start;
    return saturate(dot(uv - start, axis) / max(dot(axis, axis), 0.000001));
}

// Opening is now explicitly staged:
//   1. A thick Aether scar traces top-right -> bottom-left.
//   2. That scar pries open into the three authored wound chambers.
//   3. Only then do the preview realities appear inside.
//
// We still use one continuous diagonal frontier, but the trace, widening and
// content reveal are deliberately separated so the result reads like Arthur
// slashed the screen first and reality opened second.
float opening_frontier(float phase) {
    return mix(-0.12, 1.26, saturate(phase));
}

float opening_visibility(float along, float phase) {
    float distance_behind = opening_frontier(phase) - along;
    return smoothstep(-0.018, 0.050, distance_behind);
}

float opening_age(float along, float phase) {
    float distance_behind = opening_frontier(phase) - along;
    return smoothstep(0.0, 0.18, distance_behind);
}

float trace_phase(float opening) {
    // Pass 10: user requested a longer, more legible opening. Let the slash
    // spend more of the timeline tracing before widening takes over.
    return ease_out_cubic(saturate(opening / 0.50));
}

float widen_phase(float opening) {
    return ease_out_cubic(saturate((opening - 0.46) / 0.26));
}

float content_phase(float opening) {
    return ease_out_cubic(saturate((opening - 0.76) / 0.18));
}

float wound_open_amount(float along, float trace, float widen) {
    float traced = opening_visibility(along, trace);
    float maturity = opening_age(along, trace);
    float eased_widen = mix(0.0, widen, traced);
    return eased_widen * mix(0.86, 1.0, maturity);
}

float opened_field(float field, float along, float trace, float widen) {
    // Before widening begins, the authored chambers are held tightly shut so
    // the viewer first reads ONE thick diagonal scar. The initial closed scar
    // in Pass 09 is intentionally beefier, so we do not over-open the chambers
    // too early; the later widen phase still does the real prying apart.
    float inset = mix(0.092, 0.0, wound_open_amount(along, trace, widen));
    return field + inset;
}

float mask_from_field(float field) {
    float aa = max(fwidth(field) * 1.12, 0.00065);
    return 1.0 - smoothstep(-aa, aa, field);
}

vec2 distorted_uv(vec2 uv, float field, float opening, float apply) {
    float edge_local = 1.0 - smoothstep(0.0, 0.036, abs(field));
    float strength = edge_local * opening * (1.0 - apply);
    vec2 distortion = vec2(
        sin(uv.y * 35.0 + uv.x * 11.0),
        cos(uv.y * 23.0 - uv.x * 17.0)
    ) * (0.0030 * strength);
    return clamp(uv + distortion, 0.0, 1.0);
}


void main() {
    // Exact cancel endpoint: transparent host, real committed wallpaper below.
    if (openProgress <= 0.0005 && commitProgress <= 0.0005) {
        fragColor = vec4(0.0);
        return;
    }
    // Exact post-commit endpoint after the residual slash has burned away.
    if (finishProgress >= 0.9995) {
        fragColor = vec4(0.0);
        return;
    }

    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2 point = vec2(v_texcoord.x * aspect, v_texcoord.y);
    float opening = ease_out_cubic(openProgress);
    float apply = ease_out_cubic(commitProgress);
    float finish = saturate(finishProgress);

    // Opening is sequenced, not simultaneous: first the slash traces in, then
    // the wound widens, then the three realities reveal inside.
    float trace = trace_phase(opening);
    float widen = widen_phase(opening);
    float content = content_phase(opening);
    vec2 authored_point = point;
    float along = scar_progress(v_texcoord);
    float scar_visibility = opening_visibility(along, trace);

    // Enter is still a travelling zipper. Apply may narrow the authored cavity
    // fields, while opening uses the local frontier/age above to grow them.
    float width_scale = mix(1.0, 0.34, apply);
    float base_top_field = top_world_field(authored_point, aspect, width_scale);
    float base_previous_field = previous_world_field(
        authored_point, aspect, width_scale
    );
    float base_selected_field = selected_world_field(
        authored_point, aspect, width_scale
    );
    float base_next_field = next_world_field(
        authored_point, aspect, width_scale
    );
    float top_field = base_top_field;
    float previous_field = base_previous_field;
    float selected_field = base_selected_field;
    float next_field = base_next_field;

    // Pass 39: the silhouette is locked, so navigation may finally consume the
    // progress that WorldscarOverlay has been animating all along. The fields
    // below move the EXISTING texture roles toward the exact resting fields they
    // will own after complete_navigation() rotates the six-slot ring. nav == 0
    // is therefore pixel-identical to the accepted Pass 38 resting silhouette.
    float nav = saturate(navigationProgress);
    if (navigationDirection > 0.5) {
        // DOWN:
        // previousFar exits; previous -> top; selected -> previous; next ->
        // selected; nextFar enters the vacated bottom chamber.
        previous_field = mix(base_previous_field, base_top_field, nav);
        selected_field = mix(base_selected_field, base_previous_field, nav);
        next_field = mix(base_next_field, base_selected_field, nav);
    } else if (navigationDirection < -0.5) {
        // UP mirrors the visible three-role promotion:
        // previousFar -> previous; previous -> selected; selected -> next; the
        // old next exits. The brand-new far-top texture is loaded by the ring
        // after rotation, so only that small far chamber may still refresh at
        // the endpoint; the selected wallpaper itself no longer snaps.
        top_field = mix(base_top_field, base_previous_field, nav);
        previous_field = mix(base_previous_field, base_selected_field, nav);
        selected_field = mix(base_selected_field, base_next_field, nav);
    }

    // Opening locally narrows only the pixels just behind the travelling front.
    // At openProgress=1 every authored field is exactly full width.
    float opened_top_field = opened_field(top_field, along, trace, widen);
    float opened_previous_field = opened_field(
        previous_field, along, trace, widen
    );
    float opened_selected_field = opened_field(
        selected_field, along, trace, widen
    );
    float opened_next_field = opened_field(next_field, along, trace, widen);
    float opened_base_top_field = opened_field(
        base_top_field, along, trace, widen
    );
    float opened_base_previous_field = opened_field(
        base_previous_field, along, trace, widen
    );
    float opened_base_next_field = opened_field(
        base_next_field, along, trace, widen
    );

    // A travelling closure frontier removes the wound from top-right to
    // bottom-left during Enter. This remains independent of the opening front.
    float frontier = mix(-0.16, 1.08, apply);
    float preview_visibility = smoothstep(
        frontier - 0.045,
        frontier + 0.060,
        along
    );
    float thumbnail_visibility = scar_visibility *
        smoothstep(0.04, 0.65, content) * preview_visibility;
    float top_mask = mask_from_field(opened_top_field) *
        previousFarReady * thumbnail_visibility;
    float previous_mask = mask_from_field(opened_previous_field) *
        previousReady * thumbnail_visibility;
    float selected_mask = mask_from_field(opened_selected_field) *
        thumbnail_visibility;
    float next_mask = mask_from_field(opened_next_field) *
        nextReady * thumbnail_visibility;

    // Pass 41 makes the hidden cushion symmetric: nextFar is the resident
    // look-ahead for Down and previousFarFar is the resident look-behind for Up.
    // Neither incoming edge chamber depends on an in-transition decode anymore.
    float incoming_previous = navigationDirection < -0.5
        ? smoothstep(0.08, 0.86, nav) * previousFarFarReady
        : 0.0;
    float incoming_next = navigationDirection > 0.5
        ? smoothstep(0.08, 0.86, nav) * nextFarReady
        : 0.0;
    float incoming_previous_mask = mask_from_field(opened_base_top_field) *
        thumbnail_visibility * incoming_previous;
    float incoming_next_mask = mask_from_field(opened_base_next_field) *
        thumbnail_visibility * incoming_next;

    // The role falling off the visible four-chamber strip fades while its
    // replacement is already moving into place. This avoids double imagery at
    // the endpoint without changing any resting geometry.
    if (navigationDirection > 0.5) top_mask *= (1.0 - nav);
    if (navigationDirection < -0.5) next_mask *= (1.0 - nav);

    // Each reality is a real thumbnail, not a screen-sized wallpaper sampled
    // through a tiny hole. Local cavity UVs show a recognisable composition and
    // make the 960px decode budget useful. During navigation the UV frame moves
    // with the wallpaper so the image does not appear pinned behind the morph.
    vec2 top_screen_uv = v_texcoord;
    vec2 previous_screen_uv = v_texcoord;
    vec2 selected_screen_uv = v_texcoord;
    vec2 next_screen_uv = v_texcoord;

    vec2 top_local = preview_local_uv(
        top_screen_uv, previous_far_preview_bounds()
    );
    vec2 previous_local = preview_local_uv(
        previous_screen_uv, previous_preview_bounds()
    );
    vec2 selected_local = preview_local_uv(
        selected_screen_uv, selected_preview_bounds()
    );
    vec2 next_local = preview_local_uv(
        next_screen_uv, next_preview_bounds()
    );

    vec2 top_sample_uv = top_local;
    vec2 previous_sample_uv = previous_local;
    vec2 selected_sample_uv = selected_local;
    vec2 next_sample_uv = next_local;
    vec4 previous_sample_rect = previousUv;
    vec4 selected_sample_rect = candidateUv;
    vec4 next_sample_rect = nextUv;

    if (navigationDirection > 0.5) {
        // DOWN: move each wallpaper's local framing with its destination cavity.
        previous_sample_uv = mix(
            previous_local,
            preview_local_uv(previous_screen_uv, previous_far_preview_bounds()),
            nav
        );
        selected_sample_uv = mix(
            selected_local,
            preview_local_uv(selected_screen_uv, previous_preview_bounds()),
            nav
        );
        selected_sample_rect = mix(
            candidateUv, selectedAsPreviousUv, nav
        );
        next_sample_uv = mix(
            next_local,
            preview_local_uv(next_screen_uv, selected_preview_bounds()),
            nav
        );
        next_sample_rect = mix(nextUv, nextAsSelectedUv, nav);
    } else if (navigationDirection < -0.5) {
        // UP: previousFar descends; previous becomes selected; selected becomes
        // next. Crop rectangles interpolate to the exact post-rotation role.
        top_sample_uv = mix(
            top_local,
            preview_local_uv(top_screen_uv, previous_preview_bounds()),
            nav
        );
        previous_sample_uv = mix(
            previous_local,
            preview_local_uv(previous_screen_uv, selected_preview_bounds()),
            nav
        );
        previous_sample_rect = mix(
            previousUv, previousAsSelectedUv, nav
        );
        selected_sample_uv = mix(
            selected_local,
            preview_local_uv(selected_screen_uv, next_preview_bounds()),
            nav
        );
        selected_sample_rect = mix(candidateUv, selectedAsNextUv, nav);
    }

    vec4 previous_colour = vec4(0.0);
    vec4 selected_colour = vec4(0.0);
    vec4 next_colour = vec4(0.0);
    vec4 previous_far_colour = vec4(0.0);
    vec4 incoming_previous_colour = vec4(0.0);
    vec4 next_far_colour = vec4(0.0);
    if (previous_mask > 0.0005) {
        previous_colour = texture(
            previousTex,
            crop_uv(previous_sample_rect, previous_sample_uv)
        );
    }
    if (selected_mask > 0.0005) {
        selected_colour = texture(
            candidateTex,
            crop_uv(selected_sample_rect, selected_sample_uv)
        );
    }
    if (next_mask > 0.0005) {
        next_colour = texture(
            nextTex,
            crop_uv(next_sample_rect, next_sample_uv)
        );
    }
    if (top_mask > 0.0005) {
        previous_far_colour = texture(
            previousFarTex,
            crop_uv(previousFarUv, top_sample_uv)
        );
    }
    if (incoming_previous_mask > 0.0005) {
        incoming_previous_colour = texture(
            previousFarFarTex,
            crop_uv(previousFarFarUv, top_local)
        );
    }
    if (incoming_next_mask > 0.0005) {
        next_far_colour = texture(
            nextFarTex,
            crop_uv(nextFarUv, next_local)
        );
    }

    vec4 top_layer = vec4(
        previous_far_colour.rgb * previous_far_colour.a,
        previous_far_colour.a
    ) * top_mask;
    vec4 previous_layer = vec4(
        previous_colour.rgb * previous_colour.a,
        previous_colour.a
    ) * previous_mask;
    vec4 next_layer = vec4(
        next_colour.rgb * next_colour.a,
        next_colour.a
    ) * next_mask;
    vec4 selected_layer = vec4(
        selected_colour.rgb * selected_colour.a,
        selected_colour.a
    ) * selected_mask;
    vec4 incoming_previous_layer = vec4(
        incoming_previous_colour.rgb * incoming_previous_colour.a,
        incoming_previous_colour.a
    ) * incoming_previous_mask;
    vec4 incoming_next_layer = vec4(
        next_far_colour.rgb * next_far_colour.a,
        next_far_colour.a
    ) * incoming_next_mask;

    float top_edge_weight = previousFarReady;
    float next_edge_weight = nextReady;
    if (navigationDirection > 0.5) top_edge_weight *= (1.0 - nav);
    if (navigationDirection < -0.5) next_edge_weight *= (1.0 - nav);
    float top_edge_field = mix(10.0, opened_top_field, top_edge_weight);
    float previous_edge_field = mix(
        10.0, opened_previous_field, previousReady
    );
    float next_edge_field = mix(10.0, opened_next_field, next_edge_weight);
    float incoming_previous_edge_field = mix(
        10.0, opened_base_top_field, incoming_previous
    );
    float incoming_next_edge_field = mix(
        10.0, opened_base_next_field, incoming_next
    );
    float combined_field = min(
        opened_selected_field,
        min(
            min(top_edge_field, min(previous_edge_field, next_edge_field)),
            min(incoming_previous_edge_field, incoming_next_edge_field)
        )
    );
    float combined_mask = max(
        selected_mask,
        max(
            max(top_mask, max(previous_mask, next_mask)),
            max(incoming_previous_mask, incoming_next_mask)
        )
    );
    // The locked masks above remain untouched. Material FX sample a mildly
    // warped copy of the exact same authored/nav/opening fields, which damages
    // the perceived edge without moving a single chamber pixel.
    float base_edge_distance = abs(combined_field);
    vec2 fx_point = authored_point;
    float fx_field = combined_field;
    float coarse_fbm = 0.50;
    float fine_fbm = 0.50;
    if (base_edge_distance < 0.095) {
        float warp_gate = 1.0 - smoothstep(0.060, 0.095, base_edge_distance);
        fx_point += worldscar_domain_warp(authored_point, timeSeconds) * warp_gate;

        float fx_base_top_field = top_world_field(fx_point, aspect, width_scale);
        float fx_base_previous_field = previous_world_field(
            fx_point, aspect, width_scale
        );
        float fx_base_selected_field = selected_world_field(
            fx_point, aspect, width_scale
        );
        float fx_base_next_field = next_world_field(fx_point, aspect, width_scale);
        float fx_top_field = fx_base_top_field;
        float fx_previous_field = fx_base_previous_field;
        float fx_selected_field = fx_base_selected_field;
        float fx_next_field = fx_base_next_field;

        if (navigationDirection > 0.5) {
            fx_previous_field = mix(
                fx_base_previous_field, fx_base_top_field, nav
            );
            fx_selected_field = mix(
                fx_base_selected_field, fx_base_previous_field, nav
            );
            fx_next_field = mix(
                fx_base_next_field, fx_base_selected_field, nav
            );
        } else if (navigationDirection < -0.5) {
            fx_top_field = mix(fx_base_top_field, fx_base_previous_field, nav);
            fx_previous_field = mix(
                fx_base_previous_field, fx_base_selected_field, nav
            );
            fx_selected_field = mix(
                fx_base_selected_field, fx_base_next_field, nav
            );
        }

        float fx_opened_top_field = opened_field(
            fx_top_field, along, trace, widen
        );
        float fx_opened_previous_field = opened_field(
            fx_previous_field, along, trace, widen
        );
        float fx_opened_selected_field = opened_field(
            fx_selected_field, along, trace, widen
        );
        float fx_opened_next_field = opened_field(
            fx_next_field, along, trace, widen
        );
        float fx_opened_base_top_field = opened_field(
            fx_base_top_field, along, trace, widen
        );
        float fx_opened_base_next_field = opened_field(
            fx_base_next_field, along, trace, widen
        );

        float fx_top_edge_field = mix(
            10.0, fx_opened_top_field, top_edge_weight
        );
        float fx_previous_edge_field = mix(
            10.0, fx_opened_previous_field, previousReady
        );
        float fx_next_edge_field = mix(
            10.0, fx_opened_next_field, next_edge_weight
        );
        float fx_incoming_previous_edge_field = mix(
            10.0, fx_opened_base_top_field, incoming_previous
        );
        float fx_incoming_next_edge_field = mix(
            10.0, fx_opened_base_next_field, incoming_next
        );
        fx_field = min(
            fx_opened_selected_field,
            min(
                min(
                    fx_top_edge_field,
                    min(fx_previous_edge_field, fx_next_edge_field)
                ),
                min(
                    fx_incoming_previous_edge_field,
                    fx_incoming_next_edge_field
                )
            )
        );

        coarse_fbm = worldscar_fbm(
            fx_point * vec2(13.0, 10.0) +
            vec2(timeSeconds * 0.045, -timeSeconds * 0.060)
        );
        fine_fbm = worldscar_fbm(
            fx_point * vec2(34.0, 27.0) +
            vec2(-timeSeconds * 0.090, timeSeconds * 0.075) + 11.0
        );
    }

    float edge_phase = scar_progress_from_point(fx_point, aspect);

    float outside_distance = max(fx_field, 0.0);
    float inside_distance = max(-fx_field, 0.0);
    float edge_distance = abs(fx_field);

    float wound_visibility = scar_visibility * preview_visibility;
    float edge_visibility = wound_visibility * smoothstep(0.18, 0.72, widen);

    // 4-8 px-ish torn darkness INSIDE the previews. This is the piece that
    // visually erodes the smooth mask without needing another geometry pass.
    float motion_energy =
        smoothstep(0.02, 0.28, nav) * (1.0 - smoothstep(0.62, 1.0, nav));
    motion_energy = max(
        motion_energy,
        smoothstep(0.10, 0.92, widen) * (1.0 - smoothstep(0.72, 1.0, widen)) *
            0.40
    );

    float sine_flow = aether_turbulence(
        fx_point,
        edge_phase * 11.0 + nav * 2.7 + timeSeconds * 0.42
    );
    float structured_flow = saturate(coarse_fbm * fine_fbm * 2.05);
    float flow = mix(sine_flow, structured_flow, 0.74);
    float hotspot = smoothstep(0.48, 0.84, flow);
    float hot_node = smoothstep(
        0.62,
        0.92,
        coarse_fbm * 0.62 + fine_fbm * 0.58
    );
    float pulse = travelling_aether_pulse(edge_phase, timeSeconds);
    float idle_breathe = 0.78 + 0.22 * sin(timeSeconds * 1.75 + edge_phase * 9.0);

    float inner_lip =
        (1.0 - smoothstep(0.0015, 0.0145, inside_distance)) *
        combined_mask * edge_visibility * mix(0.88, 1.16, flow);

    // Mostly 2-5 px pale crack core. Break the intensity into irregular runs
    // rather than varying the width into a fat glowing sticker border.
    float crack_mod = 0.64 + 0.36 * sin(
        edge_phase * 41.0 + fx_point.y * 47.0 - fx_point.x * 13.0
    );
    crack_mod = smoothstep(0.24, 0.86, crack_mod);
    float rim_break = smoothstep(
        0.30,
        0.78,
        saturate(coarse_fbm * 0.72 + fine_fbm * 0.42 + crack_mod * 0.34)
    );
    float crack_core =
        (1.0 - smoothstep(0.00025, 0.0031, edge_distance)) *
        edge_visibility * mix(0.22, 1.0, rim_break) *
        mix(0.90, 1.24, flow) * mix(0.90, 1.18, pulse);

    // A sharper inner filament lives inside the main crack. This is the piece
    // that makes the wound feel more like an energized rupture than a simple
    // pale outline.
    float crack_filament =
        (1.0 - smoothstep(0.00012, 0.0019, edge_distance)) *
        edge_visibility * mix(0.66, 1.00, hotspot) *
        mix(0.86, 1.25, pulse);

    // Faint 10-15px-ish bloom lives OUTSIDE the crack. It should support the
    // boundary, never become the boundary itself.
    float outer_bloom =
        (1.0 - smoothstep(0.0030, 0.0240, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility *
        mix(0.88, 1.38, flow + motion_energy * 0.35) * idle_breathe;

    // Soft ionized haze just inside the wound. This gives the worlds an aether
    // backlight instead of a flat dark cutout at the edge.
    float inner_sheen =
        (1.0 - smoothstep(0.0020, 0.0320, inside_distance)) *
        combined_mask * edge_visibility *
        mix(0.32, 0.82, flow) *
        (0.84 + motion_energy * 0.55) * mix(0.90, 1.18, pulse);

    // Wider exterior corona used sparingly. It should feel like charged air,
    // not a sticker glow.
    float corona =
        (1.0 - smoothstep(0.0070, 0.0520, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility *
        mix(0.24, 0.62, hotspot) *
        (0.82 + motion_energy * 0.70) * idle_breathe;

    float pulse_core =
        (1.0 - smoothstep(0.0002, 0.0068, edge_distance)) *
        edge_visibility * pulse;
    float pulse_halo =
        (1.0 - smoothstep(0.0040, 0.0300, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility * pulse;

    float wisp_gate = smoothstep(
        0.60,
        0.94,
        0.50 + 0.50 * sin(
            edge_phase * 58.0 - timeSeconds * 3.4 +
            authored_point.y * 31.0
        )
    );
    float exterior_wisp =
        (1.0 - smoothstep(0.0100, 0.0380, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility * wisp_gate *
        mix(0.32, 0.80, hotspot);

    // Void-style multiplicative density, but wrapped around the Worldscar SDF
    // instead of a circular radius. This is the broad turbulent violet plasma
    // visible around the target reference's fracture rim.
    float plasma_envelope =
        (1.0 - smoothstep(0.006, 0.052, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility;
    float plasma_density = saturate(coarse_fbm * fine_fbm * 2.35 - 0.18);
    float plasma = plasma_envelope * plasma_density *
        (0.78 + 0.22 * sin(timeSeconds * 0.95 + edge_phase * 7.0));

    // Procedural branches extend from the real boundary. They are sparse and
    // forked, not a uniform halo, so the shape approaches the target's violent
    // web-of-reality-cracks look without touching cavity geometry.
    float procedural_branch = procedural_edge_fractures(
        edge_phase,
        outside_distance,
        fx_point,
        timeSeconds
    ) * (1.0 - combined_mask) * edge_visibility;
    float procedural_branch_core = procedural_branch *
        (1.0 - smoothstep(0.000, 0.060, outside_distance));
    float procedural_branch_aura = procedural_branch *
        (1.0 - smoothstep(0.012, 0.080, outside_distance));

    // Local white-hot junctions create the starburst-like nodes visible in the
    // target reference where reality appears under the most stress.
    float node_core =
        (1.0 - smoothstep(0.00015, 0.0048, edge_distance)) *
        edge_visibility * hot_node * mix(0.72, 1.0, pulse);
    float node_aura =
        (1.0 - smoothstep(0.0030, 0.0340, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility * hot_node;

    // Give the bright fracture a dark substrate. On bright wallpapers/desktops
    // this contrast is what stops the Worldscar from reading like a lavender
    // sticker outline and starts making it read like a cut through reality.
    float outer_trench =
        (1.0 - smoothstep(0.0015, 0.0155, outside_distance)) *
        (1.0 - combined_mask) * edge_visibility *
        mix(0.72, 1.0, 0.5 + 0.5 * coarse_fbm);

    // A real fracture needs visible *body*, not just a one-pixel bright outline.
    // This band straddles the accepted outer boundary on both sides: deep violet
    // underneath, structured plasma above it, then the existing white-hot core.
    float border_rift_body =
        (1.0 - smoothstep(0.0020, 0.0185, edge_distance)) *
        edge_visibility * mix(0.82, 1.0, 0.35 + 0.65 * coarse_fbm);
    float border_rift_plasma =
        (1.0 - smoothstep(0.0040, 0.0290, edge_distance)) *
        edge_visibility *
        saturate(coarse_fbm * 0.72 + fine_fbm * 0.58 - 0.28) *
        (0.84 + 0.16 * sin(timeSeconds * 1.35 + edge_phase * 15.0));

    // Internal chamber borders are not part of combined_field because the
    // cavity union swallows them. Render them as a dedicated fracture system so
    // the selected/upper and selected/bottom seams read like the target art.
    float seam_field = min(
        internal_seam_field(authored_point, aspect),
        internal_seam_fork_field(authored_point, aspect)
    );
    float seam_positive = max(seam_field, 0.0);
    float seam_aa = max(fwidth(seam_field), 0.00032);
    float seam_visibility = wound_visibility * smoothstep(0.18, 0.72, widen);
    // The physical seam stays in the same resting position; gently calm it at
    // mid-navigation while the wallpaper cavities redistribute, then bring it
    // back at the endpoint without a pop.
    float seam_nav_dip = 1.0 - 0.58 * sin(nav * 3.14159265);
    seam_visibility *= seam_nav_dip;

    float seam_fbm = 0.50;
    if (seam_positive < 0.055) {
        seam_fbm = worldscar_fbm(
            authored_point * vec2(27.0, 23.0) +
            vec2(timeSeconds * 0.050, -timeSeconds * 0.072) + 19.0
        );
    }
    float seam_pulse = 0.78 + 0.22 * sin(
        timeSeconds * 2.55 +
        authored_point.x * 29.0 - authored_point.y * 17.0
    );
    float seam_trench =
        (1.0 - smoothstep(0.0015, 0.0220, seam_positive)) *
        seam_visibility * mix(0.84, 1.0, seam_fbm);
    float seam_aura =
        (1.0 - smoothstep(0.0030, 0.0300, seam_positive)) *
        seam_visibility * mix(0.66, 1.0, seam_fbm);
    float seam_mid =
        (1.0 - smoothstep(0.0008, 0.0105, seam_positive)) *
        seam_visibility * mix(0.82, 1.0, seam_fbm);
    float seam_core =
        (1.0 - smoothstep(0.0, max(seam_aa * 2.4, 0.0024), seam_positive)) *
        seam_visibility * seam_pulse;

    float seam_node_distance = internal_seam_node_distance(
        authored_point,
        aspect
    );
    float seam_node_core =
        (1.0 - smoothstep(0.0020, 0.0100, seam_node_distance)) *
        seam_visibility;
    float seam_node_aura =
        (1.0 - smoothstep(0.0060, 0.0360, seam_node_distance)) *
        seam_visibility;

    float splinter_field = border_splinter_field(authored_point, aspect);
    float splinter_positive = max(splinter_field, 0.0);
    float splinter_aa = max(fwidth(splinter_field), 0.00030);
    float splinter_trench =
        (1.0 - smoothstep(0.0010, 0.0120, splinter_positive)) *
        edge_visibility;
    float splinter_core =
        (1.0 - smoothstep(0.0, splinter_aa * 2.0, splinter_positive)) *
        edge_visibility;

    float authored_web_field = authored_fracture_web_field(
        authored_point,
        aspect
    );
    float authored_web_positive = max(authored_web_field, 0.0);
    float authored_web_aa = max(fwidth(authored_web_field), 0.00034);
    float authored_web_core =
        (1.0 - smoothstep(0.0, authored_web_aa * 1.75, authored_web_positive)) *
        (1.0 - combined_mask) * edge_visibility;
    float authored_web_mid =
        (1.0 - smoothstep(0.0015, 0.0095, authored_web_positive)) *
        (1.0 - combined_mask) * edge_visibility;
    float authored_web_aura =
        (1.0 - smoothstep(0.0050, 0.0270, authored_web_positive)) *
        (1.0 - combined_mask) * edge_visibility;

    float authored_node_distance = authored_fracture_node_distance(
        authored_point,
        aspect
    );
    float authored_node_core =
        (1.0 - smoothstep(0.0020, 0.0090, authored_node_distance)) *
        edge_visibility;
    float authored_node_aura =
        (1.0 - smoothstep(0.0060, 0.0380, authored_node_distance)) *
        edge_visibility;
    float authored_node_pulse = 0.82 + 0.18 * sin(
        timeSeconds * 3.7 + edge_phase * 31.0
    );

    vec3 hot_aether = vec3(0.94, 0.88, 1.00);
    vec3 white_aether = vec3(0.985, 0.965, 1.00);
    vec3 aether = vec3(0.46, 0.22, 0.94);
    vec3 deep_aether = vec3(0.045, 0.010, 0.120);
    // Retained only for the existing Enter residual slash below. The new
    // Worldscar border itself is strictly violet -> pale-violet -> white.
    vec3 aether_gold = vec3(1.00, 0.69, 0.24);

    // Phase 1 material reset: preserve the authored functions for controlled
    // rollback, but remove every straight segment family from the visible
    // compositor while the organic SDF edge becomes the sole topology source.
    const float authored_segment_alpha = 0.0;

    // Phase 3: one physical cross-section, not twelve overlapping smoothsteps.
    // The trench is intentionally asymmetric: deeper inside the revealed worlds
    // and slightly wider outside, so the rim reads as torn depth instead of ink.
    float trench_outer =
        (1.0 - smoothstep(0.0010, 0.0140, outside_distance)) *
        (1.0 - combined_mask);
    float trench_inner =
        (1.0 - smoothstep(0.0008, 0.0105, inside_distance)) * combined_mask;
    float trench_layer_alpha = max(trench_outer * 0.78, trench_inner * 0.92) *
        edge_visibility * mix(0.82, 1.0, coarse_fbm);
    vec3 trench_layer_colour = mix(
        vec3(0.006, 0.001, 0.020),
        deep_aether * 0.48,
        0.44 + coarse_fbm * 0.18
    );
    vec4 reduced_trench_layer = vec4(
        trench_layer_colour * trench_layer_alpha,
        trench_layer_alpha
    );

    // Saturated violet fracture body. It carries most of the contour, allowing
    // the white-hot ridge to disappear for long stretches without losing shape.
    float body_shape = 1.0 - smoothstep(0.0008, 0.0098, edge_distance);
    float body_texture = smoothstep(
        0.24,
        0.76,
        coarse_fbm * 0.72 + fine_fbm * 0.34
    );
    float violet_body_layer_alpha = body_shape * edge_visibility *
        mix(0.68, 0.94, body_texture);
    vec3 violet_body_colour = mix(
        vec3(0.175, 0.024, 0.550),
        vec3(0.520, 0.125, 1.000),
        body_texture * 0.66 + hotspot * 0.18
    );
    vec4 violet_body_layer = vec4(
        violet_body_colour * violet_body_layer_alpha,
        violet_body_layer_alpha
    );

    // A 1-3 px ridge appears in broken runs. Noise-hot junctions may broaden it
    // locally, but there is no longer a continuous white tube around every mask.
    float core_run_signal =
        0.50 +
        0.25 * sin(edge_phase * 37.0 + fx_point.y * 31.0) +
        (fine_fbm - 0.50) * 0.72;
    float core_run = smoothstep(0.42, 0.64, core_run_signal);
    float core_ridge = 1.0 - smoothstep(0.00010, 0.00190, edge_distance);
    float node_ridge =
        (1.0 - smoothstep(0.0002, 0.0045, edge_distance)) * hot_node;
    float hot_core_layer_alpha = max(
        core_ridge * mix(0.30, 1.00, core_run),
        node_ridge * 0.94
    ) * edge_visibility * mix(0.90, 1.10, pulse);
    vec3 hot_core_colour = mix(
        vec3(0.82, 0.64, 1.00),
        white_aether,
        saturate(0.24 + core_run * 0.64 + hot_node * 0.72)
    );
    vec4 hot_core_layer = vec4(
        hot_core_colour * hot_core_layer_alpha,
        hot_core_layer_alpha
    );

    // Charged air only where coherent noise says the wound is under stress.
    // Its reach remains subordinate to the dark trench and saturated body.
    float corona_presence = smoothstep(
        0.52,
        0.78,
        coarse_fbm * 0.58 + fine_fbm * 0.52
    );
    float corona_shape =
        (1.0 - smoothstep(0.0050, 0.0300, outside_distance)) *
        (1.0 - combined_mask);
    float corona_layer_alpha = corona_shape * edge_visibility *
        corona_presence * (0.18 + motion_energy * 0.08) * idle_breathe;
    vec3 reduced_corona_colour = mix(aether, hot_aether, hotspot * 0.34);
    vec4 reduced_corona_layer = vec4(
        reduced_corona_colour * corona_layer_alpha,
        corona_layer_alpha
    );

    float ember_energy = worldscar_ember_field(
        fx_point,
        outside_distance,
        timeSeconds
    ) * (1.0 - combined_mask) * edge_visibility;
    float ember_alpha = saturate(ember_energy) * 0.90;
    vec3 ember_colour = mix(
        vec3(0.58, 0.20, 1.00),
        white_aether,
        smoothstep(0.20, 0.90, ember_energy)
    );
    vec4 ember_layer = vec4(ember_colour * ember_alpha, ember_alpha);

    // Preserve a restrained inner reaction without washing the wallpaper edge
    // into the same pale lavender as the exterior material.
    float interior_bleed =
        (1.0 - smoothstep(0.0008, 0.0075, inside_distance)) *
        combined_mask * edge_visibility;
    float edge_tint = interior_bleed * (0.08 + hotspot * 0.08);
    selected_layer.rgb = mix(
        selected_layer.rgb,
        content_reactive_rim(selected_colour.rgb) * selected_layer.a,
        edge_tint
    );
    top_layer.rgb = mix(
        top_layer.rgb,
        content_reactive_rim(previous_far_colour.rgb) * top_layer.a,
        edge_tint * 0.72
    );
    previous_layer.rgb = mix(
        previous_layer.rgb,
        content_reactive_rim(previous_colour.rgb) * previous_layer.a,
        edge_tint * 0.72
    );
    next_layer.rgb = mix(
        next_layer.rgb,
        content_reactive_rim(next_colour.rgb) * next_layer.a,
        edge_tint * 0.72
    );

    float depth_alpha = inner_lip * (0.92 + hotspot * 0.06);
    vec4 depth_layer = vec4(deep_aether * depth_alpha, depth_alpha);

    float sheen_alpha = inner_sheen * 0.34;
    vec3 sheen_colour = mix(aether, hot_aether, hotspot * 0.34 + 0.24);
    vec4 inner_sheen_layer = vec4(sheen_colour * sheen_alpha, sheen_alpha);

    float bloom_alpha = outer_bloom * 0.40;
    vec3 bloom_colour = mix(aether, hot_aether, 0.26 + hotspot * 0.24);
    vec4 glow_layer = vec4(bloom_colour * bloom_alpha, bloom_alpha);

    float corona_alpha = corona * 0.34;
    vec3 corona_colour = mix(aether, white_aether, 0.12 + hotspot * 0.34);
    vec4 corona_layer = vec4(corona_colour * corona_alpha, corona_alpha);

    float plasma_alpha = plasma * 0.44;
    vec3 plasma_colour = mix(
        deep_aether * 2.4,
        mix(aether, hot_aether, 0.48),
        saturate(plasma_density * 0.88 + hotspot * 0.20)
    );
    vec4 plasma_layer = vec4(plasma_colour * plasma_alpha, plasma_alpha);

    float trench_alpha = outer_trench * 0.58;
    vec3 trench_colour = mix(
        deep_aether * 0.42,
        vec3(0.010, 0.002, 0.030),
        0.52
    );
    vec4 trench_layer = vec4(trench_colour * trench_alpha, trench_alpha);

    float border_body_alpha = border_rift_body * 0.76;
    vec3 border_body_colour = mix(
        vec3(0.012, 0.002, 0.038),
        deep_aether * 1.75,
        0.58
    );
    vec4 border_body_layer = vec4(
        border_body_colour * border_body_alpha,
        border_body_alpha
    );

    float border_plasma_alpha = border_rift_plasma * 0.32;
    vec3 border_plasma_colour = mix(
        aether * 0.90,
        hot_aether * 1.08,
        saturate(fine_fbm * 0.76 + hotspot * 0.30)
    );
    vec4 border_plasma_layer = vec4(
        border_plasma_colour * border_plasma_alpha,
        border_plasma_alpha
    );

    float seam_trench_alpha = seam_trench * 0.86 * authored_segment_alpha;
    vec3 seam_trench_colour = mix(
        vec3(0.008, 0.001, 0.025),
        deep_aether * 1.60,
        0.64
    );
    vec4 seam_trench_layer = vec4(
        seam_trench_colour * seam_trench_alpha,
        seam_trench_alpha
    );

    float seam_aura_alpha = seam_aura * 0.54 * authored_segment_alpha;
    vec3 seam_aura_colour = mix(
        deep_aether * 1.45,
        aether * 1.35,
        0.72
    );
    vec4 seam_aura_layer = vec4(
        seam_aura_colour * seam_aura_alpha,
        seam_aura_alpha
    );

    float seam_mid_alpha = max(seam_mid * 0.86, seam_aura * 0.20) *
        authored_segment_alpha;
    vec3 seam_mid_colour = mix(aether, hot_aether, 0.52);
    vec4 seam_mid_layer = vec4(
        seam_mid_colour * seam_mid_alpha,
        seam_mid_alpha
    );

    float seam_core_alpha = max(seam_core * 0.98, seam_node_core * 0.92) *
        authored_segment_alpha;
    vec3 seam_core_colour = mix(
        hot_aether,
        white_aether,
        saturate(0.74 + seam_fbm * 0.26)
    );
    vec4 seam_core_layer = vec4(
        seam_core_colour * seam_core_alpha,
        seam_core_alpha
    );

    float seam_node_alpha = max(
        seam_node_aura * 0.42,
        seam_node_core * 1.00
    ) * (0.88 + 0.12 * sin(timeSeconds * 3.6)) * authored_segment_alpha;
    vec3 seam_node_colour = mix(hot_aether, white_aether, 0.94);
    vec4 seam_node_layer = vec4(
        seam_node_colour * seam_node_alpha,
        seam_node_alpha
    );

    float splinter_trench_alpha = splinter_trench * 0.62 *
        authored_segment_alpha;
    vec3 splinter_trench_colour = vec3(0.010, 0.002, 0.032);
    vec4 splinter_trench_layer = vec4(
        splinter_trench_colour * splinter_trench_alpha,
        splinter_trench_alpha
    );
    float splinter_core_alpha = splinter_core *
        (0.82 + 0.18 * sin(timeSeconds * 3.2 + edge_phase * 17.0)) *
        authored_segment_alpha;
    vec3 splinter_core_colour = mix(hot_aether, white_aether, 0.86);
    vec4 splinter_core_layer = vec4(
        splinter_core_colour * splinter_core_alpha,
        splinter_core_alpha
    );

    float authored_web_shadow_alpha = authored_web_aura * 0.34 *
        authored_segment_alpha;
    vec3 authored_web_shadow_colour = deep_aether * 1.65;
    vec4 authored_web_shadow_layer = vec4(
        authored_web_shadow_colour * authored_web_shadow_alpha,
        authored_web_shadow_alpha
    );

    float authored_web_mid_alpha = max(
        authored_web_mid * 0.72,
        authored_web_aura * 0.26
    ) * authored_segment_alpha;
    vec3 authored_web_mid_colour = mix(aether, hot_aether, 0.38);
    vec4 authored_web_mid_layer = vec4(
        authored_web_mid_colour * authored_web_mid_alpha,
        authored_web_mid_alpha
    );

    float authored_web_core_alpha = authored_web_core *
        (0.94 + 0.06 * sin(timeSeconds * 5.2 + edge_phase * 23.0)) *
        authored_segment_alpha;
    vec3 authored_web_core_colour = mix(hot_aether, white_aether, 0.86);
    vec4 authored_web_core_layer = vec4(
        authored_web_core_colour * authored_web_core_alpha,
        authored_web_core_alpha
    );

    float authored_node_alpha = max(
        authored_node_aura * 0.56,
        authored_node_core * 1.00
    ) * authored_node_pulse * authored_segment_alpha;
    vec3 authored_node_colour = mix(hot_aether, white_aether, 0.93);
    vec4 authored_node_layer = vec4(
        authored_node_colour * authored_node_alpha,
        authored_node_alpha
    );

    // Procedural rays use the same physical cross-section as the main wound:
    // a dark substrate survives busy wallpapers, then a narrower pale core
    // carries the electrical read. Topology remains sparse and noise-authored.
    float procedural_branch_trench_alpha = procedural_branch_aura * 0.70;
    vec3 procedural_branch_trench_colour = vec3(0.008, 0.001, 0.026);
    vec4 procedural_branch_trench_layer = vec4(
        procedural_branch_trench_colour * procedural_branch_trench_alpha,
        procedural_branch_trench_alpha
    );
    float procedural_branch_hot = smoothstep(
        0.38,
        0.78,
        procedural_branch_core
    );
    float procedural_branch_alpha = max(
        procedural_branch_aura * 0.12,
        procedural_branch_hot * 0.95
    );
    vec3 procedural_branch_colour = mix(
        vec3(0.72, 0.38, 1.00),
        vec3(0.98, 0.92, 1.00),
        saturate(0.26 + procedural_branch_core * 0.68 + hot_node * 0.24)
    );
    vec4 procedural_branch_layer = vec4(
        procedural_branch_colour * procedural_branch_alpha,
        procedural_branch_alpha
    );

    float node_alpha = max(node_aura * 0.34, node_core * 1.00);
    vec3 node_colour = mix(hot_aether, white_aether, 0.88);
    vec4 node_layer = vec4(node_colour * node_alpha, node_alpha);

    float pulse_alpha = max(pulse_core * 0.82, pulse_halo * 0.34);
    vec3 pulse_colour = mix(hot_aether, white_aether, 0.78);
    vec4 pulse_layer = vec4(pulse_colour * pulse_alpha, pulse_alpha);

    float wisp_alpha = exterior_wisp * 0.24;
    vec3 wisp_colour = mix(aether, hot_aether, 0.48);
    vec4 wisp_layer = vec4(wisp_colour * wisp_alpha, wisp_alpha);

    float core_alpha = max(
        max(crack_core * 0.98, crack_filament * 0.92),
        node_core * 0.98
    );
    vec3 core_colour = mix(hot_aether, white_aether, crack_mod * 0.72);
    core_colour = mix(
        core_colour,
        white_aether,
        saturate(crack_filament * 0.44 + hot_node * 0.36)
    );
    vec4 edge_crack_layer = vec4(core_colour * core_alpha, core_alpha);


    // Sparse lightning that genuinely protrudes away from the mask. This is
    // intentionally separate from the edge distance so it can visually break
    // the rounded contour instead of tracing it faithfully.
    float edge_branch_field = edge_lightning_field(authored_point, aspect);
    float branch_aa = max(fwidth(edge_branch_field), 0.00040);
    float branch_core =
        (1.0 - smoothstep(0.0, branch_aa * 1.55, max(edge_branch_field, 0.0))) *
        edge_visibility;
    float branch_aura =
        (1.0 - smoothstep(0.0010, 0.0100, max(edge_branch_field, 0.0))) *
        edge_visibility;
    float branch_breathe = 0.86 + 0.14 * sin(timeSeconds * 3.1 + edge_phase * 12.0);
    float branch_alpha = max(
        branch_aura * (0.14 + motion_energy * 0.05),
        branch_core * (0.90 + motion_energy * 0.08)
    ) * branch_breathe * authored_segment_alpha;
    vec3 branch_colour = mix(
        hot_aether,
        white_aether,
        saturate(branch_core * 0.82 + hotspot * 0.10)
    );
    vec4 branch_layer = vec4(branch_colour * branch_alpha, branch_alpha);

    // Opening stage 1: a thick, screen-slashed scar traces downward before the
    // wound fully opens. Pass 10: make the trace thicker again and let it hold
    // the visual read longer before thumbnails show up.
    float forming_scar_field = opening_scar_field(point, aspect) -
        mix(0.0048, 0.0018, widen);
    float forming_scar_body = mask_from_field(forming_scar_field) *
        scar_visibility * (1.0 - apply) * (1.0 - content * 0.78);
    float forming_scar_edge =
        (1.0 - smoothstep(0.0, 0.0068, abs(forming_scar_field))) *
        scar_visibility * (1.0 - apply) * (1.0 - content * 0.56);
    float forming_scar_aura =
        (1.0 - smoothstep(0.004, 0.056, max(forming_scar_field, 0.0))) *
        scar_visibility * (1.0 - apply) * (1.0 - content * 0.62);
    float forming_scar_alpha = max(
        forming_scar_body * 0.76,
        max(forming_scar_aura * 0.30, forming_scar_edge * 1.00)
    );
    vec3 forming_scar_colour = mix(
        vec3(0.074, 0.016, 0.190),
        mix(aether, hot_aether, 0.68),
        saturate(forming_scar_edge + forming_scar_aura * 0.30)
    );
    vec4 forming_scar_layer = vec4(
        forming_scar_colour * forming_scar_alpha,
        forming_scar_alpha
    );

    // The zipper leaves a clearly visible wound behind it. A dark violet body
    // gives the slash physical thickness; a pale violet/gold edge and wider aura
    // stop it reading as the near-invisible hairline from Pass 03.
    float slash_field = residual_slash_field(point, aspect);
    float slash_outer = max(slash_field, 0.0);
    float slash_trail = (1.0 - preview_visibility) *
        smoothstep(0.015, 0.16, apply) * (1.0 - finish);
    float slash_body = mask_from_field(slash_field) * slash_trail;
    float slash_edge =
        (1.0 - smoothstep(0.0, 0.0030, abs(slash_field))) * slash_trail;
    float slash_aura =
        (1.0 - smoothstep(0.002, 0.030, slash_outer)) * slash_trail;

    float gold_mix = 0.28 + 0.34 *
        sin(v_texcoord.y * 19.0 + screen_uv(v_texcoord).x * 11.0);
    vec3 slash_body_colour = vec3(0.105, 0.025, 0.255);
    vec3 slash_hot_colour = mix(
        hot_aether,
        aether_gold,
        saturate(gold_mix)
    );
    float slash_alpha = max(
        slash_body * 0.78,
        max(slash_aura * 0.30, slash_edge * 0.96)
    );
    vec3 slash_colour = mix(
        slash_body_colour,
        slash_hot_colour,
        saturate(slash_edge + slash_aura * 0.22)
    );
    vec4 slash_layer = vec4(slash_colour * slash_alpha, slash_alpha);

    vec4 composed = reduced_corona_layer;
    composed = forming_scar_layer + composed * (1.0 - forming_scar_layer.a);
    composed = incoming_previous_layer +
        composed * (1.0 - incoming_previous_layer.a);
    composed = incoming_next_layer + composed * (1.0 - incoming_next_layer.a);
    // Keep the selected chamber above the upper preview: Pass 31 proved that
    // reversing this order simply clips away the crown instead of fixing it.
    composed = previous_layer + composed * (1.0 - previous_layer.a);
    composed = selected_layer + composed * (1.0 - selected_layer.a);
    composed = next_layer + composed * (1.0 - next_layer.a);
    composed = top_layer + composed * (1.0 - top_layer.a);
    // The four-role cross-section sits above the previews: dark recess,
    // saturated material, then intermittent hot ridge. No stacked fog bands.
    composed = reduced_trench_layer +
        composed * (1.0 - reduced_trench_layer.a);
    composed = violet_body_layer + composed * (1.0 - violet_body_layer.a);
    composed = hot_core_layer + composed * (1.0 - hot_core_layer.a);
    // Rays and motes are exterior-only, so placing them last cannot contaminate
    // the locked previews; it only prevents busy wallpaper layers from burying
    // the sparse detail they are meant to support.
    composed = procedural_branch_trench_layer +
        composed * (1.0 - procedural_branch_trench_layer.a);
    composed = procedural_branch_layer +
        composed * (1.0 - procedural_branch_layer.a);
    composed = ember_layer + composed * (1.0 - ember_layer.a);
    composed = slash_layer + composed * (1.0 - slash_layer.a);

    fragColor = composed;
}
