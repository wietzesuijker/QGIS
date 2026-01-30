#version 450

// Fragment shader for RGBA raster rendering
// Direct RGBA output with opacity

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTileTexture;

layout(std140, binding = 0) uniform Params {
    mat4 uMVPMatrix;
    float uScale;
    float uMinValue;
    float uMaxValue;
    float uNoDataValue;
    float uOpacity;
    float uUseNoData;
    vec2 _padding;
};

void main() {
    vec4 color = texture(uTileTexture, vTexCoord);
    fragColor = vec4(color.rgb, color.a * uOpacity);
}
