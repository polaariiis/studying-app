#version 330 core
// Textured quads (rasterised text, images). docs/RENDERING.md §6.2.
//
// The mesh is the unit square [0, 1]²; uModel maps it to the draw space like solid.vert,
// and the square's coordinates are the texture coordinates (texture row 0 at y = 0).
// Depth orders content parts exactly as in solid.vert: each item covers a pixel once.

layout(location = 0) in vec2 aPosition;

uniform mat3 uModel;
uniform vec4 uProjection;
uniform float uFirstPart;

const float kDepthParts = 1048576.0; // 2^20, as in solid.vert

out vec2 vUv;

void main() {
    vUv = aPosition;
    vec2 space = (uModel * vec3(aPosition, 1.0)).xy;
    float depth = (uFirstPart + 1.0) / kDepthParts;
    gl_Position = vec4(space * uProjection.xy + uProjection.zw, 2.0 * depth - 1.0, 1.0);
}
