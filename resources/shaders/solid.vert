#version 330 core
// Solid meshes (strokes, shapes, overlays). docs/RENDERING.md §6.2.
//
// uModel maps mesh space to the draw space: camera-relative world units for content,
// view pixels for overlays. uProjection maps the draw space to clip space as
// clip = position * uProjection.xy + uProjection.zw (the y flip lives in uProjection).

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aColor; // per-vertex colour (batches); ignored otherwise

uniform mat3 uModel;
uniform vec4 uProjection;
uniform int uVertexColors;

out vec4 vColor;

void main() {
    vColor = uVertexColors != 0 ? aColor : vec4(1.0);
    vec2 space = (uModel * vec3(aPosition, 1.0)).xy;
    gl_Position = vec4(space * uProjection.xy + uProjection.zw, 0.0, 1.0);
}
