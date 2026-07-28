// Realmheart Void — original shell transition prototype.
// A transparent aetheric core grows from the target centre while a torn
// violet/gold event horizon consumes the source texture. Open playback runs
// the same field in reverse so the target reforms from its outer edges.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform float progress;
uniform vec2 resolution;
uniform sampler2D tex;
uniform float radius;
uniform float reverse;

uniform vec3 uGold;
uniform vec3 uStarlight;
uniform vec3 uAstral;
uniform vec3 uVoid;

layout(location = 0) out vec4 fragColor;

float hash21(vec2 point) {
    point = fract(point * vec2(123.34, 456.21));
    point += dot(point, point + 45.32);
    return fract(point.x * point.y);
}

float valueNoise(vec2 point) {
    vec2 cell = floor(point);
    vec2 local = fract(point);
    vec2 blend = local * local * (3.0 - 2.0 * local);

    float a = hash21(cell);
    float b = hash21(cell + vec2(1.0, 0.0));
    float c = hash21(cell + vec2(0.0, 1.0));
    float d = hash21(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
}

float turbulence(vec2 point) {
    float result = 0.0;
    float amplitude = 0.55;
    mat2 turn = mat2(0.82, -0.57, 0.57, 0.82);
    for (int octave = 0; octave < 4; ++octave) {
        result += valueNoise(point) * amplitude;
        point = turn * point * 2.07 + vec2(7.1, 3.7);
        amplitude *= 0.5;
    }
    return result;
}

float roundedBoxSdf(vec2 point, vec2 halfSize, float cornerRadius) {
    vec2 delta = abs(point) - halfSize + cornerRadius;
    return length(max(delta, 0.0)) + min(max(delta.x, delta.y), 0.0) - cornerRadius;
}

vec4 premultipliedOver(vec4 under, vec4 over) {
    return vec4(
        over.rgb + under.rgb * (1.0 - over.a),
        over.a + under.a * (1.0 - over.a)
    );
}

void main() {
    vec2 safeResolution = max(resolution, vec2(1.0));
    vec2 uv = v_texcoord;
    vec4 source = texture(tex, uv);

    if (radius > 0.0) {
        vec2 pixel = uv * safeResolution;
        float clipDistance = roundedBoxSdf(
            pixel - safeResolution * 0.5,
            safeResolution * 0.5,
            radius
        );
        source *= 1.0 - smoothstep(-1.0, 1.0, clipDistance);
    }

    float phase = mix(progress, 1.0 - progress, reverse);
    phase = smoothstep(0.0, 1.0, clamp(phase, 0.0, 1.0));

    float aspect = safeResolution.x / safeResolution.y;
    vec2 centred = (uv - 0.5) * vec2(aspect, 1.0);
    float distanceFromCore = length(centred);
    float cornerDistance = length(vec2(0.5 * aspect, 0.5));

    float angle = atan(centred.y, centred.x);
    float edgeNoise = turbulence(
        centred * 5.2 + vec2(cos(angle * 3.0), sin(angle * 2.0)) * 0.55
    );
    float tearing = (edgeNoise - 0.5) * mix(0.04, 0.19, phase);

    float coreRadius = cornerDistance * (0.02 + 1.12 * pow(phase, 1.24));
    float signedEdge = distanceFromCore - (coreRadius + tearing);

    float sourceMask = smoothstep(-0.018, 0.018, signedEdge);
    vec4 survivingSource = source * sourceMask;

    float horizonWidth = mix(0.055, 0.14, phase);
    float horizon = exp(-pow(signedEdge / horizonWidth, 2.0));
    float innerRim = exp(-pow((signedEdge + horizonWidth * 0.28) / 0.018, 2.0));
    float outerMist = exp(-pow((signedEdge - horizonWidth * 0.35) / (horizonWidth * 1.65), 2.0));

    float grain = turbulence(centred * 10.0 - vec2(phase * 3.6, phase * 1.9));
    float smokeShape = horizon * smoothstep(0.2, 0.88, grain + horizon * 0.24);
    float horizonAlpha = clamp(smokeShape * 0.72 + innerRim * 0.88 + outerMist * 0.22, 0.0, 0.94);

    vec3 smokeColour = mix(uAstral * 1.15, uVoid * 2.2, smoothstep(0.18, 0.9, grain));
    smokeColour = mix(smokeColour, uStarlight * 1.25, innerRim * 0.58);
    smokeColour += uGold * innerRim * (0.20 + 0.28 * hash21(floor(uv * safeResolution / 6.0)));

    float speck = step(0.992, hash21(floor(uv * safeResolution / 3.0) + floor(progress * 23.0)));
    smokeColour += (uStarlight + uGold * 0.35) * speck * horizon * 1.7;

    float endpointFade = smoothstep(0.0, 0.035, progress) *
        (1.0 - smoothstep(0.965, 1.0, progress));
    vec4 eventHorizon = vec4(
        smokeColour * horizonAlpha * endpointFade,
        horizonAlpha * endpointFade
    );

    fragColor = premultipliedOver(survivingSource, eventHorizon);
}
