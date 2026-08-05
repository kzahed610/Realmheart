// Aether Sunder — Realmheart window effect.
// The source fractures into staggered diagonal ribbons. A violet-starlight
// front crosses the window while each ribbon shears away independently;
// opening is the exact reverse reconstruction.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;
in vec2 v_texcoord;

uniform float progress;
uniform vec2 resolution;
uniform sampler2D tex;
uniform float radius;
uniform float reverse; // 1.0 = OPEN, 0.0 = CLOSE

uniform vec3 uGold;
uniform vec3 uStarlight;
uniform vec3 uAstral;
uniform vec3 uVoid;

layout(location = 0) out vec4 fragColor;

float hash11(float value) {
    return fract(sin(value * 127.1) * 43758.5453123);
}

float hash12(vec2 value) {
    vec3 p3 = fract(vec3(value.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float roundedBoxSDF(vec2 center, vec2 halfSize, float r) {
    vec2 d = abs(center) - halfSize + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

void main() {
    vec2 uv = v_texcoord;

    float bandCount = clamp(floor(resolution.y / 72.0), 10.0, 18.0);
    float bandId = floor(uv.y * bandCount);
    float bandRandom = hash11(bandId + 13.7);

    // Closing consumes 0 -> 1. Opening receives the exact reverse timeline.
    float consume = mix(progress, 1.0 - progress, reverse);

    // Every band shears by a tiny independent amount while disappearing.
    // The endpoint guard below guarantees an exact, undistorted source frame.
    float drift = (bandRandom - 0.5) * 0.042 * pow(consume, 1.35);
    vec2 sampleUv = clamp(uv + vec2(drift, 0.0), vec2(0.0), vec2(1.0));
    vec4 source = texture(tex, sampleUv);

    float shapeMask = 1.0;
    if (radius > 0.0) {
        vec2 pixelPos = uv * resolution;
        float sd = roundedBoxSDF(
            pixelPos - resolution * 0.5,
            resolution * 0.5,
            radius
        );
        shapeMask = 1.0 - smoothstep(-1.0, 1.0, sd);
    }

    // Hard terminal frames prevent a one-frame handoff snap in either direction.
    if (reverse > 0.5 && progress >= 1.0) {
        fragColor = source * shapeMask;
        return;
    }
    if (reverse < 0.5 && progress >= 1.0) {
        fragColor = vec4(0.0);
        return;
    }
    if (reverse > 0.5 && progress <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // A staggered diagonal front crosses the window. The random band phase is
    // what makes the image read as separate ribbons rather than a plain wipe.
    float bandPhase = (bandRandom - 0.5) * 0.18;
    float coordinate = uv.x * 0.78 + uv.y * 0.22 + bandPhase;
    float eased = consume * consume * (3.0 - 2.0 * consume);
    float front = mix(-0.24, 1.24, eased);
    float feather = 0.026;

    float removed = 1.0 - smoothstep(
        front - feather,
        front + feather,
        coordinate
    );
    float visible = 1.0 - removed;

    float frontDistance = abs(coordinate - front);
    float frontGlow = 1.0 - smoothstep(feather * 0.35, feather * 2.8, frontDistance);

    // Thin fracture seams appear only close to the moving front.
    float bandLocal = fract(uv.y * bandCount);
    float seamDistance = min(bandLocal, 1.0 - bandLocal);
    float seam = 1.0 - smoothstep(0.0, 0.055, seamDistance);
    float fracture = seam * frontGlow;

    // Sparse gold sparks ride the front without turning it into a solid ring.
    vec2 sparkCell = floor(uv * resolution / 5.0);
    float spark = step(0.985, hash12(sparkCell + vec2(bandId, bandId * 0.37)));
    spark *= frontGlow;

    float edgeMix = clamp(frontGlow * 0.82 + fracture * 0.45, 0.0, 1.0);
    vec3 edgeColour = mix(uAstral, uStarlight, 0.52 + 0.35 * bandRandom);
    edgeColour += uGold * spark * 1.45;
    edgeColour += uVoid * fracture * 0.25;

    vec3 colour = mix(source.rgb, edgeColour, edgeMix);

    // The luminous front may exist just inside the removed region, but fades
    // before the close endpoint so the terminal frame remains fully transparent.
    float endpointFade = 1.0 - smoothstep(0.94, 1.0, consume);
    float edgeAlpha = frontGlow * endpointFade * source.a * 0.92;
    float alpha = max(source.a * visible, edgeAlpha);

    // Clip ribbons, fracture glow, and the sampled source together so no
    // procedural pixels square off the compositor's rounded corners.
    fragColor = vec4(colour, alpha) * shapeMask;
}
