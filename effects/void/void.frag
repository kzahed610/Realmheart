// Void —  window effect.
// A void seeds at the window's heart and devours it: violet-blue nebula smoke
// billows in a ring around the growing hole (TRANSPARENT — the desktop shows
// through), star specks glitter in the smoke, then everything sinks into nothing.
// OPEN is the recession: the ring recedes while the see-through iris shrinks away,
// revealing the window edges-first. After the lunar-arcanum void transition.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;
in vec2 v_texcoord;

uniform float progress;
uniform vec2 resolution;
uniform sampler2D tex;
uniform float radius;
uniform float reverse; // 1.0 = OPEN (void recedes), 0.0 = CLOSE (void swallows)

//  palette (overridable at runtime for theme-follow)
uniform vec3 uGold;
uniform vec3 uStarlight;
uniform vec3 uAstral;
uniform vec3 uVoid;

layout(location = 0) out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.55;
    for (int i = 0; i < 3; i++) {
        v += noise2(p) * a;
        p = p * 2.13 + 17.0;
        a *= 0.5;
    }
    return v;
}
float roundedBoxSDF(vec2 center, vec2 halfSize, float r) {
    vec2 d = abs(center) - halfSize + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

void main() {
    // swallow: 0 = window whole, 1 = fully devoured
    float swallow = mix(progress, 1.0 - progress, reverse);
    vec2  uv = v_texcoord;

    vec4 windowColor = texture(tex, uv);
    windowColor.a = 1.0;
    if (radius > 0.0) {
        vec2 pixelPos = uv * resolution;
        float sd = roundedBoxSDF(pixelPos - resolution * 0.5, resolution * 0.5, radius);
        windowColor.a *= 1.0 - smoothstep(-1.0, 1.0, sd);
    }

    // radial field from the window centre, aspect-corrected so the void is round
    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2  c    = (uv - 0.5) * vec2(aspect, 1.0);
    float r    = length(c);
    float rMax = length(vec2(0.5 * aspect, 0.5)); // centre -> corner

    // the core: an irregular ink-blot whose edge is torn by noise tendrils.
    // It renders TRANSPARENT — the window is genuinely devoured, the desktop
    // showing through the hole inside the nebula ring.
    float cr       = pow(swallow, 1.15) * rMax * 1.15;
    float tendril  = fbm(c * 3.2 + 7.0) - 0.5;
    float coreEdge = cr * (1.0 + tendril * 0.55);

    // nebula smoke: a billowing ring hugging the core edge, rolling inward
    float bandW = 0.16 + 0.34 * swallow;
    float dEdge = r - coreEdge;
    float env   = exp(-pow((dEdge - bandW * 0.35) / bandW, 2.0) * 1.8);
    vec2  roll  = c * 3.4 - (r > 0.0 ? c / r : vec2(0.0)) * swallow * 1.3;
    float dens  = fbm(roll + 3.0) * fbm(c * 6.5 - swallow * 2.0 + 11.0) * 2.2;
    float smoke = clamp(env * dens, 0.0, 1.0);

    // deep violet-blue billows, brightening to starlight at the inner rim
    vec3 nebC = mix(uVoid * 2.5, uAstral * 1.55, smoke);
    nebC      = mix(nebC, uAstral * 0.6 + vec3(0.06, 0.05, 0.30), 0.45); // pull toward blue-violet
    float rim = exp(-pow(dEdge / 0.045, 2.0)) * smoothstep(0.05, 0.25, swallow);
    nebC     += uStarlight * rim * smoke * 0.8;

    // star specks glittering inside the smoke band — round glints, one per sparse cell
    vec2  suv  = uv * resolution / 26.0;
    vec2  sid  = floor(suv);
    float sh   = hash12(sid + 5.1);
    vec2  sp   = sid + 0.2 + 0.6 * vec2(hash12(sid + 1.7), hash12(sid + 9.2));
    float sd2  = dot(suv - sp, suv - sp);
    float twk  = 0.55 + 0.45 * sin(swallow * 26.0 + sh * 31.0);
    float starA = step(0.72, sh) * exp(-sd2 * 60.0) * smoke * twk;
    vec3  starC = mix(uStarlight, vec3(1.0), 0.4) * starA;

    // everything sinks away at the very end of the swallow (close ends empty)
    float fade = 1.0 - smoothstep(0.85, 1.0, swallow);
    float ends = smoothstep(0.0, 0.04, progress) * (1.0 - smoothstep(0.96, 1.0, progress));

    // the window survives outside the smoke, devoured through it
    windowColor.a *= smoothstep(coreEdge, coreEdge + bandW, r) * fade;

    float ovA = clamp(smoke * 0.9 + starA, 0.0, 1.0) * fade * ends;
    vec3  ovC = (nebC * smoke + starC) * fade * ends;

    float a   = clamp(ovA + windowColor.a * (1.0 - ovA), 0.0, 1.0);
    vec3  rgb = ovC + windowColor.rgb * windowColor.a * (1.0 - ovA);
    fragColor = vec4(min(rgb, vec3(1.5)), a);
}
