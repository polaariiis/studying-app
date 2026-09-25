#version 330 core
// Procedural page background (docs/RENDERING.md §6.2): paper, bounded page edges and
// ruled / grid / dot patterns anchored in world space, computed per pixel, so there is no
// geometry per line and lines stay one device pixel wide at every zoom.
//
// Coordinates: gl_FragCoord is in framebuffer pixels with the origin at the bottom-left.
// It is converted to view pixels (top-left origin), then to camera-relative world units:
//   rel = (view - viewport / 2) / zoom
// uPhase is (camera centre mod spacing), so rel + uPhase has the same position within the
// pattern period as the true world coordinate, without float precision loss far from 0.

uniform vec2 uViewport;         // logical pixels
uniform float uDevicePixelRatio;
uniform float uFramebufferHeight;
uniform float uZoom;
uniform vec4 uDeskColor;
uniform vec4 uPaperColor;
uniform vec4 uPatternColor;
uniform int uPattern;           // 0 none, 1 ruled, 2 grid, 3 dots
uniform float uSpacing;         // world units
uniform vec2 uPhase;
uniform int uBounded;
uniform vec4 uPageRect;         // camera-relative world: min.xy, max.xy
uniform int uInvertLightness;

out vec4 fragColor;

// The result is then lifted so that white paper becomes the dark theme's canvas grey
// (#171717, lightness 0.09) rather than pure black, while black ink still becomes white.
const float kDarkPaperLightness = 0.09;
vec3 invertLightness(vec3 c) {
    float shift = 1.0 - max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    vec3 inverted = clamp(c + vec3(shift), 0.0, 1.0);
    return mix(vec3(kDarkPaperLightness), vec3(1.0), inverted);
}

// Coverage of a line of `widthPx` device pixels at signed distance `d` (world units).
float lineCoverage(float d, float widthPx) {
    float px = abs(d) * uZoom * uDevicePixelRatio;
    return 1.0 - smoothstep(widthPx * 0.5, widthPx * 0.5 + 1.0, px);
}

void main() {
    vec2 view = vec2(gl_FragCoord.x, uFramebufferHeight - gl_FragCoord.y) / uDevicePixelRatio;
    vec2 rel = (view - uViewport * 0.5) / uZoom;

    if (uBounded != 0 &&
        (rel.x < uPageRect.x || rel.y < uPageRect.y || rel.x > uPageRect.z || rel.y > uPageRect.w)) {
        fragColor = uDeskColor;
        return;
    }

    vec3 paper = uInvertLightness != 0 ? invertLightness(uPaperColor.rgb) : uPaperColor.rgb;
    float coverage = 0.0;
    if (uPattern != 0 && uSpacing > 0.0) {
        vec2 p = rel + uPhase;
        vec2 cell = p - floor(p / uSpacing + 0.5) * uSpacing; // offset to the nearest line/dot
        if (uPattern == 1) {
            coverage = lineCoverage(cell.y, 1.0);
        } else if (uPattern == 2) {
            coverage = max(lineCoverage(cell.x, 1.0), lineCoverage(cell.y, 1.0));
        } else {
            float radiusPx = 1.25 * uDevicePixelRatio;
            float px = length(cell) * uZoom * uDevicePixelRatio;
            coverage = 1.0 - smoothstep(radiusPx - 0.5, radiusPx + 0.5, px);
        }
        // Fade the pattern out before its period gets too small to read (moire).
        coverage *= smoothstep(4.0, 8.0, uSpacing * uZoom);
    }
    fragColor = vec4(mix(paper, uPatternColor.rgb, coverage * uPatternColor.a), 1.0);
}
