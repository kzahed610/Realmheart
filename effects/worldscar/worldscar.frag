#version 300 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D previousTex;
uniform sampler2D candidateTex;
uniform sampler2D nextTex;
uniform sampler2D previousFarTex;
uniform sampler2D nextFarTex;
uniform vec2 resolution;
uniform float openProgress;
uniform float commitProgress;
uniform float finishProgress;
uniform float navigationProgress;
uniform float navigationDirection;
uniform float previousReady;
uniform float nextReady;
uniform float previousFarReady;
uniform float nextFarReady;
uniform vec4 previousUv;
uniform vec4 candidateUv;
uniform vec4 nextUv;
uniform vec4 previousFarUv;
uniform vec4 nextFarUv;

// The GTK overlay intentionally owns only the left 54% of the monitor. Shader
// authoring coordinates remain screen-normalized so the wound keeps the same
// physical composition while the transparent right 46% allocates no GL buffer.
const float WORLDSCAR_SURFACE_FRACTION = 0.54;

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

vec4 previous_preview_bounds() {
    return vec4(local_x(0.265), 0.012, local_x(0.505), 0.292);
}

vec4 selected_preview_bounds() {
    return vec4(local_x(0.040), 0.315, local_x(0.478), 0.718);
}

vec4 next_preview_bounds() {
    return vec4(local_x(0.035), 0.742, local_x(0.295), 0.985);
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

float contour_damage(vec2 point, float phase) {
    return (
        0.0025 * sin(point.y * 34.0 + point.x * 7.0 + phase) +
        0.0011 * sin(point.y * 67.0 - point.x * 13.0 - phase * 0.8)
    );
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
float previous_world_field(vec2 point, float aspect, float opening) {
    float hairline = mix(0.0006, 0.0017, opening);
    float field = scar_ellipse_field(
        point,
        vec2(0.405, 0.150),
        vec2(0.108, 0.128),
        aspect
    );

    // Narrow neck toward the selected chamber.
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.390, 0.200), aspect),
            scar_point(vec2(0.345, 0.285), aspect),
            mix(hairline, 0.064, opening),
            mix(hairline, 0.020, opening)
        ),
        mix(0.002, 0.014, opening)
    );

    // Pointed crown and leftward tear make the top chamber feel ripped rather
    // than circular while keeping most of its readable area near the top-right.
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.440, 0.062), aspect),
            scar_point(vec2(0.485, 0.018), aspect),
            mix(hairline, 0.038, opening),
            mix(hairline, 0.008, opening)
        ),
        mix(0.002, 0.010, opening)
    );
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.370, 0.180), aspect),
            scar_point(vec2(0.275, 0.215), aspect),
            mix(hairline, 0.038, opening),
            mix(hairline, 0.010, opening)
        ),
        mix(0.002, 0.010, opening)
    );

    // Opposing bites create a broken, asymmetric chamber boundary.
    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.290, 0.102),
            vec2(0.090, 0.058),
            aspect
        )
    );
    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.505, 0.248),
            vec2(0.075, 0.064),
            aspect
        )
    );

    field += contour_damage(point, 1.4) * opening * 0.96;
    return field;
}

float selected_world_field(vec2 point, float aspect, float opening) {
    float hairline = mix(0.0007, 0.0020, opening);

    // The selected reality is intentionally the dominant chamber. Three
    // overlapping lobes make its mass follow the diagonal spine instead of
    // forming one giant horizontal oval.
    float field = scar_ellipse_field(
        point,
        vec2(0.345, 0.405),
        vec2(0.135, 0.105),
        aspect
    );
    field = smooth_min(
        field,
        scar_ellipse_field(
            point,
            vec2(0.285, 0.505),
            vec2(0.165, 0.150),
            aspect
        ),
        mix(0.003, 0.030, opening)
    );
    field = smooth_min(
        field,
        scar_ellipse_field(
            point,
            vec2(0.205, 0.615),
            vec2(0.155, 0.115),
            aspect
        ),
        mix(0.003, 0.026, opening)
    );

    // Upper neck from the previous reality.
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.345, 0.325), aspect),
            scar_point(vec2(0.330, 0.390), aspect),
            mix(hairline, 0.034, opening),
            mix(hairline, 0.075, opening)
        ),
        mix(0.002, 0.014, opening)
    );

    // Sparse authored shards widen the silhouette without turning it back into
    // a moon. The right shard balances the broad centre; the left shards carry
    // the eye naturally toward the terminal side of the composition.
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.365, 0.430), aspect),
            scar_point(vec2(0.475, 0.405), aspect),
            mix(hairline, 0.055, opening),
            mix(hairline, 0.010, opening)
        ),
        mix(0.002, 0.012, opening)
    );
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.200, 0.620), aspect),
            scar_point(vec2(0.045, 0.690), aspect),
            mix(hairline, 0.065, opening),
            mix(hairline, 0.010, opening)
        ),
        mix(0.002, 0.014, opening)
    );
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.235, 0.540), aspect),
            scar_point(vec2(0.100, 0.500), aspect),
            mix(hairline, 0.050, opening),
            mix(hairline, 0.008, opening)
        ),
        mix(0.002, 0.010, opening)
    );

    // Large concave bites are the important silhouette change in this pass.
    // They break the card/blob read while preserving a huge readable interior.
    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.180, 0.355),
            vec2(0.100, 0.065),
            aspect
        )
    );
    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.485, 0.545),
            vec2(0.095, 0.080),
            aspect
        )
    );
    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.095, 0.725),
            vec2(0.080, 0.050),
            aspect
        )
    );

    field += contour_damage(point, 3.0) * opening * 1.06;
    return field;
}

float next_world_field(vec2 point, float aspect, float opening) {
    float hairline = mix(0.0006, 0.0017, opening);
    float field = scar_ellipse_field(
        point,
        vec2(0.135, 0.850),
        vec2(0.115, 0.110),
        aspect
    );

    // Small neck back toward the selected chamber.
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.180, 0.745), aspect),
            scar_point(vec2(0.140, 0.825), aspect),
            mix(hairline, 0.042, opening),
            mix(hairline, 0.086, opening)
        ),
        mix(0.002, 0.016, opening)
    );

    // The lower chamber inherits the diagonal momentum and resolves toward the
    // low-X side without changing the separately-authored Enter damage slash.
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.140, 0.860), aspect),
            scar_point(vec2(0.045, 0.955), aspect),
            mix(hairline, 0.060, opening),
            mix(hairline, 0.008, opening)
        ),
        mix(0.002, 0.012, opening)
    );
    field = smooth_min(
        field,
        tapered_segment(
            point,
            scar_point(vec2(0.160, 0.820), aspect),
            scar_point(vec2(0.285, 0.790), aspect),
            mix(hairline, 0.052, opening),
            mix(hairline, 0.010, opening)
        ),
        mix(0.002, 0.012, opening)
    );

    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.295, 0.895),
            vec2(0.085, 0.065),
            aspect
        )
    );
    field = carve_notch(
        field,
        scar_ellipse_field(
            point,
            vec2(0.035, 0.775),
            vec2(0.070, 0.050),
            aspect
        )
    );

    field += contour_damage(point, 5.1) * opening * 0.96;
    return field;
}

float fracture_segment(vec2 point, vec2 start, vec2 end, float width) {
    return tapered_segment(point, start, end, width, width * 0.12);
}

float connective_fracture_field(vec2 point, float aspect) {
    float fracture = fracture_segment(
        point,
        scar_point(vec2(0.395, 0.260), aspect),
        scar_point(vec2(0.370, 0.310), aspect),
        0.0018
    );
    fracture = min(
        fracture,
        fracture_segment(
            point,
            scar_point(vec2(0.255, 0.655), aspect),
            scar_point(vec2(0.225, 0.705), aspect),
            0.0017
        )
    );
    fracture = min(
        fracture,
        fracture_segment(
            point,
            scar_point(vec2(0.360, 0.115), aspect),
            scar_point(vec2(0.245, 0.055), aspect),
            0.0013
        )
    );
    fracture = min(
        fracture,
        fracture_segment(
            point,
            scar_point(vec2(0.210, 0.590), aspect),
            scar_point(vec2(0.075, 0.570), aspect),
            0.0015
        )
    );
    fracture = min(
        fracture,
        fracture_segment(
            point,
            scar_point(vec2(0.125, 0.900), aspect),
            scar_point(vec2(0.035, 0.930), aspect),
            0.0013
        )
    );
    return fracture;
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
    float base_previous_field = previous_world_field(
        authored_point, aspect, width_scale
    );
    float base_selected_field = selected_world_field(
        authored_point, aspect, width_scale
    );
    float base_next_field = next_world_field(
        authored_point, aspect, width_scale
    );
    float previous_field = base_previous_field;
    float selected_field = base_selected_field;
    float next_field = base_next_field;

    float nav = saturate(navigationProgress);
    if (navigationDirection > 0.5) {
        // DOWN: NEXT climbs into selected, selected recedes into previous. The
        // hidden NEXT+1 is rendered into the vacated next cavity DURING this
        // same morph, so complete_navigation() has no silhouette/image pop.
        previous_field += nav * 0.18;
        selected_field = mix(selected_field, base_previous_field, nav);
        next_field = mix(next_field, base_selected_field, nav);
    } else if (navigationDirection < -0.5) {
        // UP mirrors the same pipeline with PREVIOUS+1 entering the top cavity.
        next_field += nav * 0.18;
        selected_field = mix(selected_field, base_next_field, nav);
        previous_field = mix(previous_field, base_selected_field, nav);
    }

    // Opening locally narrows only the pixels just behind the travelling front.
    // At openProgress=1 every authored field is exactly full width.
    float opened_previous_field = opened_field(
        previous_field, along, trace, widen
    );
    float opened_selected_field = opened_field(
        selected_field, along, trace, widen
    );
    float opened_next_field = opened_field(next_field, along, trace, widen);
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
    float previous_mask = mask_from_field(opened_previous_field) *
        previousReady * thumbnail_visibility;
    float selected_mask = mask_from_field(opened_selected_field) *
        thumbnail_visibility;
    float next_mask = mask_from_field(opened_next_field) *
        nextReady * thumbnail_visibility;

    // Hidden +/-1 textures already exist in the five-slot ring. Pass 05 made
    // them cheap and ready; Pass 06 finally renders the incoming one before the
    // role rotation instead of making the new neighbour appear after the morph.
    float incoming_previous = navigationDirection < -0.5
        ? smoothstep(0.10, 0.82, nav) * previousFarReady
        : 0.0;
    float incoming_next = navigationDirection > 0.5
        ? smoothstep(0.10, 0.82, nav) * nextFarReady
        : 0.0;
    float incoming_previous_mask = mask_from_field(opened_base_previous_field) *
        thumbnail_visibility * incoming_previous;
    float incoming_next_mask = mask_from_field(opened_base_next_field) *
        thumbnail_visibility * incoming_next;

    if (navigationDirection > 0.5) previous_mask *= (1.0 - nav);
    if (navigationDirection < -0.5) next_mask *= (1.0 - nav);

    // Each reality is a real thumbnail, not a screen-sized wallpaper sampled
    // through a tiny hole. Local cavity UVs show a recognisable composition and
    // make the 960px decode budget useful. During navigation the UV frame moves
    // with the wallpaper so the image does not appear pinned behind the morph.
    vec2 previous_screen_uv = distorted_uv(
        v_texcoord, previous_field, opening, apply
    );
    vec2 selected_screen_uv = distorted_uv(
        v_texcoord, selected_field, opening, apply
    );
    vec2 next_screen_uv = distorted_uv(
        v_texcoord, next_field, opening, apply
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

    vec2 previous_sample_uv = previous_local;
    vec2 selected_sample_uv = selected_local;
    vec2 next_sample_uv = next_local;
    if (navigationDirection > 0.5) {
        selected_sample_uv = mix(
            selected_local,
            preview_local_uv(selected_screen_uv, previous_preview_bounds()),
            nav
        );
        next_sample_uv = mix(
            next_local,
            preview_local_uv(next_screen_uv, selected_preview_bounds()),
            nav
        );
    } else if (navigationDirection < -0.5) {
        selected_sample_uv = mix(
            selected_local,
            preview_local_uv(selected_screen_uv, next_preview_bounds()),
            nav
        );
        previous_sample_uv = mix(
            previous_local,
            preview_local_uv(previous_screen_uv, selected_preview_bounds()),
            nav
        );
    }

    vec4 previous_colour = vec4(0.0);
    vec4 selected_colour = vec4(0.0);
    vec4 next_colour = vec4(0.0);
    vec4 previous_far_colour = vec4(0.0);
    vec4 next_far_colour = vec4(0.0);
    if (previous_mask > 0.0005) {
        previous_colour = texture(
            previousTex,
            crop_uv(previousUv, previous_sample_uv)
        );
    }
    if (selected_mask > 0.0005) {
        selected_colour = texture(
            candidateTex,
            crop_uv(candidateUv, selected_sample_uv)
        );
    }
    if (next_mask > 0.0005) {
        next_colour = texture(
            nextTex,
            crop_uv(nextUv, next_sample_uv)
        );
    }
    if (incoming_previous_mask > 0.0005) {
        previous_far_colour = texture(
            previousFarTex,
            crop_uv(previousFarUv, previous_local)
        );
    }
    if (incoming_next_mask > 0.0005) {
        next_far_colour = texture(
            nextFarTex,
            crop_uv(nextFarUv, next_local)
        );
    }

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
        previous_far_colour.rgb * previous_far_colour.a,
        previous_far_colour.a
    ) * incoming_previous_mask;
    vec4 incoming_next_layer = vec4(
        next_far_colour.rgb * next_far_colour.a,
        next_far_colour.a
    ) * incoming_next_mask;

    float previous_edge_field = mix(
        10.0, opened_previous_field, previousReady
    );
    float next_edge_field = mix(10.0, opened_next_field, nextReady);
    float incoming_previous_edge_field = mix(
        10.0, opened_base_previous_field, incoming_previous
    );
    float incoming_next_edge_field = mix(
        10.0, opened_base_next_field, incoming_next
    );
    float combined_field = min(
        opened_selected_field,
        min(
            min(previous_edge_field, next_edge_field),
            min(incoming_previous_edge_field, incoming_next_edge_field)
        )
    );
    float combined_mask = max(
        selected_mask,
        max(
            max(previous_mask, next_mask),
            max(incoming_previous_mask, incoming_next_mask)
        )
    );
    float outer_distance = max(combined_field, 0.0);
    float inner_distance = max(-combined_field, 0.0);
    float inner_edge =
        (1.0 - smoothstep(0.0, 0.0075, inner_distance)) * combined_mask;
    float outer_edge =
        (1.0 - smoothstep(0.002, 0.020, outer_distance)) *
        (1.0 - combined_mask);
    float depth_band =
        (1.0 - smoothstep(0.006, 0.047, outer_distance)) *
        (1.0 - combined_mask);

    float wound_visibility = scar_visibility * preview_visibility;
    float normal_edge_visibility = wound_visibility * mix(0.16, 1.0, widen);
    inner_edge *= normal_edge_visibility;
    outer_edge *= normal_edge_visibility;
    depth_band *= normal_edge_visibility;

    vec3 hot_aether = vec3(0.90, 0.82, 1.00);
    vec3 aether = vec3(0.49, 0.26, 0.93);
    vec3 deep_aether = vec3(0.085, 0.025, 0.225);
    vec3 aether_gold = vec3(1.00, 0.69, 0.24);

    selected_layer.rgb = mix(
        selected_layer.rgb,
        hot_aether * selected_layer.a,
        inner_edge * 0.29
    );
    previous_layer.rgb = mix(
        previous_layer.rgb,
        hot_aether * previous_layer.a,
        inner_edge * 0.16
    );
    next_layer.rgb = mix(
        next_layer.rgb,
        hot_aether * next_layer.a,
        inner_edge * 0.16
    );

    float depth_alpha = depth_band * 0.28;
    vec4 depth_layer = vec4(deep_aether * depth_alpha, depth_alpha);
    float glow_alpha = outer_edge * 0.29;
    vec3 glow_colour = mix(aether, hot_aether, outer_edge * 0.28);
    vec4 glow_layer = vec4(glow_colour * glow_alpha, glow_alpha);

    float fracture_reveal = smoothstep(0.22, 0.88, widen) *
        wound_visibility;
    float fracture_field = connective_fracture_field(authored_point, aspect);
    float fracture_aa = max(fwidth(fracture_field), 0.00050);
    float fracture_core =
        (1.0 - smoothstep(0.0, fracture_aa * 1.8, max(fracture_field, 0.0))) *
        fracture_reveal;
    float fracture_aura =
        (1.0 - smoothstep(0.001, 0.011, max(fracture_field, 0.0))) *
        fracture_reveal;
    float fracture_alpha = max(fracture_aura * 0.15, fracture_core * 0.52);
    vec3 fracture_colour = mix(aether, hot_aether, fracture_core * 0.72);
    vec4 fracture_layer = vec4(
        fracture_colour * fracture_alpha,
        fracture_alpha
    );

    // Opening stage 1: a thick, screen-slashed scar traces downward before the
    // wound fully opens. Pass 10: make the trace thicker again and let it hold
    // the visual read longer before thumbnails show up.
    float forming_scar_field = residual_slash_field(point, aspect) -
        mix(0.0360, 0.0135, widen);
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

    vec4 composed = depth_layer;
    composed = glow_layer + composed * (1.0 - glow_layer.a);
    composed = fracture_layer + composed * (1.0 - fracture_layer.a);
    composed = forming_scar_layer + composed * (1.0 - forming_scar_layer.a);
    composed = incoming_previous_layer +
        composed * (1.0 - incoming_previous_layer.a);
    composed = incoming_next_layer + composed * (1.0 - incoming_next_layer.a);
    composed = previous_layer + composed * (1.0 - previous_layer.a);
    composed = next_layer + composed * (1.0 - next_layer.a);
    composed = selected_layer + composed * (1.0 - selected_layer.a);
    composed = slash_layer + composed * (1.0 - slash_layer.a);

    fragColor = composed;
}
