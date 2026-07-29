#version 440
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D texY;
layout(binding = 2) uniform sampler2D texUV;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 params;
} ubuf;

void main() {
    // NV12: Y 平面(R8) + UV 交错平面(RG8,U=r,V=g)
    // 与 I420 的 video.frag 处理一致:Y 不加偏移、全范围,UV 减 0.5 中心。
    float y = texture(texY, v_texCoord).r;
    vec2 uv = texture(texUV, v_texCoord).rg;
    float u = uv.x - 0.5;
    float v = uv.y - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    fragColor = vec4(r, g, b, 1.0);
}