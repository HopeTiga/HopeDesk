#version 440
layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 fragColor;

// I420 三平面打包成一张 R8(w x (h+chromaH)):
//   Y 顶部 h 行 [0, yEnd)
//   U 底部 chromaH 行左半、V 右半 [yEnd, 1)
layout(binding = 1) uniform sampler2D texYuv;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 params;   // x = yEnd(Y占高比例), y = chromaScale(色度带占高比例), z = 0.5(半宽)
} ubuf;

void main() {
    float yEnd = ubuf.params.x;
    float chromaScale = ubuf.params.y;
    float halfW = ubuf.params.z;

    // Y:纹理顶部 h 行
    float y = texture(texYuv, vec2(v_texCoord.x, v_texCoord.y * yEnd)).r;

    // U/V:底部 chromaH 行,左右各半宽
    float chromaV = yEnd + v_texCoord.y * chromaScale;
    float u = texture(texYuv, vec2(v_texCoord.x * halfW, chromaV)).r - 0.5;
    float v = texture(texYuv, vec2(v_texCoord.x * halfW + halfW, chromaV)).r - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    fragColor = vec4(r, g, b, 1.0);
}
