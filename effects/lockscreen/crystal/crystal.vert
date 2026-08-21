// Broken Seal crystal shard — shared fullscreen-triangle vertex shader.
// Geometry is derived from gl_VertexID, so no VBO is required.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

out vec2 v_texcoord;

void main() {
    vec2 corner = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );
    v_texcoord = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
