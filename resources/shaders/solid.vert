#version 330 core
// Solid meshes (strokes, shapes, overlays). docs/RENDERING.md §6.2.
//
// uModel maps mesh space to the draw space: camera-relative world units for content,
// view pixels for overlays. uProjection maps the draw space to clip space as
// clip = position * uProjection.xy + uProjection.zw (the y flip lives in uProjection).
//
// Depth orders the content's parts: part k of the frame (uFirstPart plus the vertex's part
// in its mesh) is drawn at window depth (k + 1) / kDepthParts with the test "greater", so
// each part covers a pixel at most once (docs/RENDERING.md §6.5; kDepthParts in
// OpenGLRenderer.cpp). Overlays are drawn without the depth test.

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aColor; // per-vertex colour (batches); ignored otherwise
layout(location = 2) in uint aPart;  // part within the mesh (batches); ignored otherwise

uniform mat3 uModel;
uniform vec4 uProjection;
uniform int uVertexColors;
uniform float uFirstPart;
uniform int uVertexParts;

const float kDepthParts = 1048576.0; // 2^20

out vec4 vColor;

void main() {
    vColor = uVertexColors != 0 ? aColor : vec4(1.0);
    vec2 space = (uModel * vec3(aPosition, 1.0)).xy;
    float part = uFirstPart + (uVertexParts != 0 ? float(aPart) : 0.0);
    float depth = (part + 1.0) / kDepthParts; // window depth in (0, 1]
    gl_Position = vec4(space * uProjection.xy + uProjection.zw, 2.0 * depth - 1.0, 1.0);
}
