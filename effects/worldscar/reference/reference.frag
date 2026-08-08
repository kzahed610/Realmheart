#version 300 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D candidateTex;
uniform vec2 resolution;
uniform float openProgress;
uniform float commitProgress;
uniform vec4 candidateUv;

vec2 crop_uv(vec4 rect, vec2 uv) {
    return mix(rect.xy, rect.zw, uv);
}

float ease_out_cubic(float value) {
    float inverse = 1.0 - clamp(value, 0.0, 1.0);
    return 1.0 - inverse * inverse * inverse;
}

void main() {
    // Closed/cancel endpoint: the Worldscar surface contributes no pixels at
    // all. The real Realmheart wallpaper renderer remains the base world.
    if (openProgress <= 0.0005 && commitProgress <= 0.0005) {
        fragColor = vec4(0.0);
        return;
    }

    vec4 candidateColor = texture(
        candidateTex,
        crop_uv(candidateUv, v_texcoord)
    );

    // Apply endpoint: Worldscar owns an exact full-screen candidate frame
    // while the real wallpaper backend catches up underneath it.
    if (commitProgress >= 0.9995) {
        fragColor = candidateColor;
        return;
    }

    float open = ease_out_cubic(openProgress);
    float commit = ease_out_cubic(commitProgress);

    // Still intentionally plain. The only composition decision we lock here
    // is that the primary wound belongs left of centre like the reference.
    vec2 p = v_texcoord - vec2(0.355, 0.50);
    p.x *= resolution.x / max(resolution.y, 1.0);

    float angle = -0.28;
    float c = cos(angle);
    float s = sin(angle);
    vec2 q = mat2(c, -s, s, c) * p;

    float halfWidth = mix(0.004, 0.20, open);
    float halfHeight = mix(0.30, 0.54, open);
    halfWidth = mix(halfWidth, 2.4, commit);
    halfHeight = mix(halfHeight, 2.4, commit);

    vec2 normalized = q / vec2(max(halfWidth, 0.001), max(halfHeight, 0.001));
    float distanceToOpening = length(normalized) - 1.0;
    float feather = mix(0.018, 0.004, commit);
    float mask = 1.0 - smoothstep(-feather, feather, distanceToOpening);

    // Keep RGB premultiplied by coverage so the transparent edge does not
    // leave dark/bright fringes when GTK hands the RGBA surface to Wayland.
    fragColor = candidateColor * mask;
}
