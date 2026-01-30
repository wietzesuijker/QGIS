#version 450

// Fragment shader for paletted/indexed raster rendering
// Looks up color from palette texture using pixel value as index

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTileTexture;
layout(binding = 2) uniform sampler2D uPaletteTexture;

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
    // Index is stored as normalized value (0-1 representing 0-255)
    float index = texture(uTileTexture, vTexCoord).r;

    // Lookup color from 256x1 palette texture
    vec4 color = texture(uPaletteTexture, vec2(index, 0.5));
    fragColor = vec4(color.rgb, color.a * uOpacity);
}
