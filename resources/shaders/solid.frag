#version 330 core
// Straight-alpha colour; optional display colour transform (docs/RENDERING.md §8).

uniform vec4 uColor;           // multiplies the per-vertex colour (white when absent)
uniform int uInvertLightness;

in vec4 vColor;

out vec4 fragColor;

// HSL lightness inversion that keeps hue and saturation: shifting all channels by
// 1 - max - min maps lightness L = (max + min) / 2 to 1 - L. Black ink becomes white on
// dark paper; saturated colours keep their hue.
// The result is then lifted so that white paper becomes the dark theme's canvas grey
// (#171717, lightness 0.09) rather than pure black, while black ink still becomes white.
const float kDarkPaperLightness = 0.09;
vec3 invertLightness(vec3 c) {
    float shift = 1.0 - max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    vec3 inverted = clamp(c + vec3(shift), 0.0, 1.0);
    return mix(vec3(kDarkPaperLightness), vec3(1.0), inverted);
}

void main() {
    vec4 color = vColor * uColor;
    vec3 rgb = uInvertLightness != 0 ? invertLightness(color.rgb) : color.rgb;
    fragColor = vec4(rgb, color.a);
}
