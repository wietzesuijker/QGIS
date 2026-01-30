#version 450

// Fragment shader for single-channel raster with colormap lookup
// Used for Byte, UInt16, and Float32 data types
//
// Contrast enhancement algorithms (uAlgorithm):
// 0 = NoEnhancement: passthrough (scale to 0-1)
// 1 = StretchToMinimumMaximum: linear stretch, saturate at bounds
// 2 = ClipToMinimumMaximum: discard pixels outside [min,max]
// 3 = StretchAndClipToMinimumMaximum: clip then linear stretch

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTileTexture;
layout(binding = 2) uniform sampler2D uColormapTexture;

// Uniform block layout must match C++ UniformBlock struct (std140)
layout(std140, binding = 0) uniform Params {
    mat4 uMVPMatrix;       // 64 bytes, offset 0
    float uScale;          // Data type scale (4 bytes, offset 64)
    float uOpacity;        // Layer opacity (4 bytes, offset 68)
    float uUseNoData;      // 1.0 if NoData should be checked (4 bytes, offset 72)
    float uAlgorithm;      // Contrast enhancement algorithm (4 bytes, offset 76)
    // Per-band min/max (single-band uses R channel values only)
    float uMinR;           // Min value for normalization (4 bytes, offset 80)
    float uMaxR;           // Max value for normalization (4 bytes, offset 84)
    float uMinG;           // (unused in colormap mode) (4 bytes, offset 88)
    float uMaxG;           // (unused in colormap mode) (4 bytes, offset 92)
    float uMinB;           // (unused in colormap mode) (4 bytes, offset 96)
    float uMaxB;           // (unused in colormap mode) (4 bytes, offset 100)
    // Per-band NoData values (single-band uses R channel only)
    float uNoDataR;        // NoData value (4 bytes, offset 104)
    float uNoDataG;        // (unused) (4 bytes, offset 108)
    float uNoDataB;        // (unused) (4 bytes, offset 112)
    float uNoDataTolerance; // NoData comparison tolerance (4 bytes, offset 116)
    vec2 _padding;         // Alignment (8 bytes, offset 120)
    // Total: 128 bytes
};

void main() {
    float rawValue = texture(uTileTexture, vTexCoord).r;
    float value = rawValue * uScale;

    // NoData check with configurable tolerance
    if (uUseNoData > 0.5 && abs(value - uNoDataR) < uNoDataTolerance) {
        discard;
    }

    float normalized;

    if (uAlgorithm < 0.5) {
        // NoEnhancement (algorithm 0): passthrough
        normalized = rawValue;
    }
    else if (uAlgorithm < 1.5) {
        // StretchToMinimumMaximum (algorithm 1): linear stretch, saturate at bounds
        float range = uMaxR - uMinR;
        normalized = (range > 0.001) ? clamp((value - uMinR) / range, 0.0, 1.0) : rawValue;
    }
    else {
        // ClipToMinimumMaximum (algorithm 2) or StretchAndClipToMinimumMaximum (algorithm 3)

        // Discard pixels outside the min/max range
        if (value < uMinR || value > uMaxR) {
            discard;
        }

        if (uAlgorithm < 2.5) {
            // ClipToMinimumMaximum (algorithm 2): clip only, no stretch
            normalized = rawValue;
        }
        else {
            // StretchAndClipToMinimumMaximum (algorithm 3): clip then stretch
            float range = uMaxR - uMinR;
            normalized = (range > 0.001) ? (value - uMinR) / range : rawValue;
        }
    }

    // 1D colormap texture lookup (stored as 256x1 2D texture)
    vec4 color = texture(uColormapTexture, vec2(normalized, 0.5));
    fragColor = vec4(color.rgb, color.a * uOpacity);
}
