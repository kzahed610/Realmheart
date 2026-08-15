#version 330 core

// Radial bloom shader for Mana Cores apply animation
// Reveals wallpaper from core centre outward

uniform vec2 u_origin;        // Core centre in NDC (-1..1)
uniform float u_radius;       // Current reveal radius (0..2.0, where sqrt(2) = corner)
uniform sampler2D u_wallpaper; // Wallpaper texture
uniform float u_alpha;        // Overall alpha (1.0 = fully visible)

in vec2 v_uv;
out vec4 fragColor;

void main() {
    // Distance from core centre in NDC
    float d = length(v_uv * 2.0 - 1.0 - u_origin);

    // Smooth step for anti-aliased edge
    float edge_width = 0.01;
    float reveal = smoothstep(u_radius - edge_width, u_radius, d);

    vec4 wp = texture(u_wallpaper, v_uv);
    fragColor = vec4(wp.rgb, wp.a * reveal * u_alpha);
}