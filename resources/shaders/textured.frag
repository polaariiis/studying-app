#version 330 core
// Premultiplied RGBA texture, tinted by uColor, written as straight alpha (the content
// blend function); optional display colour transform as in solid.frag.

uniform sampler2D uTexture;
uniform vec4 uColor;
uniform int uInvertLightness;

in vec2 vUv;

out vec4 fragColor;

const float kDarkPaperLightness = 0.09;
vec3 invertLightness(vec3 c) {
    float shift = 1.0 - max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    vec3 inverted = clamp(c + vec3(shift), 0.0, 1.0);
    return mix(vec3(kDarkPaperLightness), vec3(1.0), inverted);
}

void main() {
    vec4 texel = texture(uTexture, vUv);
    if (texel.a <= 0.0) {
        discard; // transparent: neither colour nor depth
    }
    vec3 straight = texel.rgb / texel.a;
    vec4 color = vec4(straight, texel.a) * uColor;
    vec3 rgb = uInvertLightness != 0 ? invertLightness(color.rgb) : color.rgb;
    fragColor = vec4(rgb, color.a);
}
