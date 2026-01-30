#version 450

// Fragment shader for RGB raster rendering
// Applies per-band contrast enhancement and NoData handling
//
// Contrast enhancement algorithms (uAlgorithm):
// 0 = NoEnhancement: passthrough (scale to 0-1)
// 1 = StretchToMinimumMaximum: linear stretch, saturate at bounds
// 2 = ClipToMinimumMaximum: discard pixels outside [min,max]
// 3 = StretchAndClipToMinimumMaximum: clip then linear stretch

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTileTexture;

// Uniform block layout must match C++ UniformBlock struct (std140)
layout(std140, binding = 0) uniform Params {
    mat4 uMVPMatrix;       // 64 bytes, offset 0
    float uScale;          // Data type scale (4 bytes, offset 64)
    float uOpacity;        // Layer opacity (4 bytes, offset 68)
    float uUseNoData;      // 1.0 if NoData should be checked (4 bytes, offset 72)
    float uAlgorithm;      // Contrast enhancement algorithm (4 bytes, offset 76)
    // Per-band min/max for contrast enhancement
    float uMinR;           // Red min (4 bytes, offset 80)
    float uMaxR;           // Red max (4 bytes, offset 84)
    float uMinG;           // Green min (4 bytes, offset 88)
    float uMaxG;           // Green max (4 bytes, offset 92)
    float uMinB;           // Blue min (4 bytes, offset 96)
    float uMaxB;           // Blue max (4 bytes, offset 100)
    // Per-band NoData values
    float uNoDataR;        // Red NoData (4 bytes, offset 104)
    float uNoDataG;        // Green NoData (4 bytes, offset 108)
    float uNoDataB;        // Blue NoData (4 bytes, offset 112)
    float uNoDataTolerance; // NoData comparison tolerance (4 bytes, offset 116)
    vec2 _padding;         // Alignment (8 bytes, offset 120)
    // Total: 128 bytes
};

void main() {
    vec3 rawColor = texture(uTileTexture, vTexCoord).rgb;

    // Scale from normalized texture values to actual data values
    vec3 value = rawColor * uScale;

    // Per-band NoData check
    if (uUseNoData > 0.5) {
        if (abs(value.r - uNoDataR) < uNoDataTolerance ||
            abs(value.g - uNoDataG) < uNoDataTolerance ||
            abs(value.b - uNoDataB) < uNoDataTolerance) {
            discard;
        }
    }

    vec3 stretched;

    if (uAlgorithm < 0.5) {
        // NoEnhancement (algorithm 0): passthrough, just scale to 0-1
        stretched = rawColor;
    }
    else if (uAlgorithm < 1.5) {
        // StretchToMinimumMaximum (algorithm 1): linear stretch, saturate at bounds
        float rangeR = uMaxR - uMinR;
        float rangeG = uMaxG - uMinG;
        float rangeB = uMaxB - uMinB;

        stretched.r = (rangeR > 0.001) ? clamp((value.r - uMinR) / rangeR, 0.0, 1.0) : rawColor.r;
        stretched.g = (rangeG > 0.001) ? clamp((value.g - uMinG) / rangeG, 0.0, 1.0) : rawColor.g;
        stretched.b = (rangeB > 0.001) ? clamp((value.b - uMinB) / rangeB, 0.0, 1.0) : rawColor.b;
    }
    else {
        // ClipToMinimumMaximum (algorithm 2) or StretchAndClipToMinimumMaximum (algorithm 3)
        // Both clip outside range; algorithm 3 also stretches within range

        // Discard pixels outside the min/max range
        if (value.r < uMinR || value.r > uMaxR ||
            value.g < uMinG || value.g > uMaxG ||
            value.b < uMinB || value.b > uMaxB) {
            discard;
        }

        if (uAlgorithm < 2.5) {
            // ClipToMinimumMaximum (algorithm 2): clip only, no stretch
            // Normalize from data type range to 0-1
            stretched = rawColor;
        }
        else {
            // StretchAndClipToMinimumMaximum (algorithm 3): clip then stretch
            float rangeR = uMaxR - uMinR;
            float rangeG = uMaxG - uMinG;
            float rangeB = uMaxB - uMinB;

            stretched.r = (rangeR > 0.001) ? (value.r - uMinR) / rangeR : rawColor.r;
            stretched.g = (rangeG > 0.001) ? (value.g - uMinG) / rangeG : rawColor.g;
            stretched.b = (rangeB > 0.001) ? (value.b - uMinB) / rangeB : rawColor.b;
        }
    }

    fragColor = vec4(stretched, uOpacity);
}
