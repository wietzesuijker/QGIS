#version 450

// Fragment shader for RGB raster rendering
// Applies per-band contrast enhancement, NoData handling, and post-processing filters
//
// Contrast enhancement algorithms (uAlgorithm):
// 0 = NoEnhancement: passthrough (scale to 0-1)
// 1 = StretchToMinimumMaximum: linear stretch, saturate at bounds
// 2 = ClipToMinimumMaximum: discard pixels outside [min,max]
// 3 = StretchAndClipToMinimumMaximum: clip then linear stretch
//
// Post-processing filters (applied in order):
// 1. Brightness/Contrast/Gamma (from QgsBrightnessContrastFilter)
// 2. Hue/Saturation (from QgsHueSaturationFilter)
//    Order: Invert -> Grayscale/Saturation -> Colorize

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTileTexture;

// Uniform block layout must match C++ UniformBlock struct (std140, 256 bytes)
layout(std140, binding = 0) uniform Params {
    mat4 uMVPMatrix;        // 64 bytes, offset 0
    float uScale;           // Data type scale (4 bytes, offset 64)
    float uOpacity;         // Layer opacity (4 bytes, offset 68)
    float uUseNoData;       // 1.0 if NoData should be checked (4 bytes, offset 72)
    float uAlgorithm;       // Contrast enhancement algorithm (4 bytes, offset 76)
    // Per-band min/max for contrast enhancement
    float uMinR;            // Red min (4 bytes, offset 80)
    float uMaxR;            // Red max (4 bytes, offset 84)
    float uMinG;            // Green min (4 bytes, offset 88)
    float uMaxG;            // Green max (4 bytes, offset 92)
    float uMinB;            // Blue min (4 bytes, offset 96)
    float uMaxB;            // Blue max (4 bytes, offset 100)
    // Per-band NoData values
    float uNoDataR;         // Red NoData (4 bytes, offset 104)
    float uNoDataG;         // Green NoData (4 bytes, offset 108)
    float uNoDataB;         // Blue NoData (4 bytes, offset 112)
    float uNoDataTolerance; // NoData comparison tolerance (4 bytes, offset 116)
    vec2 _pad1;             // Alignment (8 bytes, offset 120)
    // Subtotal: 128 bytes

    // Brightness/Contrast/Gamma filter (offset 128)
    // Formulas: contrastFactor = pow((contrast+100)/100, 2), gammaCorrection = 1/gamma
    float uBrightness;      // -255 to 255 (4 bytes, offset 128)
    float uContrastFactor;  // Precomputed contrast factor (4 bytes, offset 132)
    float uGammaCorrection; // Precomputed 1/gamma (4 bytes, offset 136)
    float _pad2;            // Alignment (4 bytes, offset 140)

    // Hue/Saturation filter (offset 144)
    // Order: Invert -> Grayscale/Saturation -> Colorize
    float uInvertColors;    // 0 or 1 (4 bytes, offset 144)
    float uGrayscaleMode;   // 0=off, 1=lightness, 2=luminosity, 3=average (4 bytes, offset 148)
    float uSaturationScale; // (saturation/100)+1, range 0-2 (4 bytes, offset 152)
    float uColorizeOn;      // 0 or 1 (4 bytes, offset 156)
    float uColorizeH;       // 0-1 hue (4 bytes, offset 160)
    float uColorizeS;       // 0-1 saturation (4 bytes, offset 164)
    float uColorizeStrength; // 0-1 strength (4 bytes, offset 168)
    float _pad3;            // Alignment (4 bytes, offset 172)

    // Filter enable flags (offset 176)
    float uUseBrightnessFilter; // 1 if filter active (4 bytes, offset 176)
    float uUseHueSatFilter;     // 1 if filter active (4 bytes, offset 180)
    // Reserved space to 256 bytes (offset 184-255)
};

// RGB to HSL conversion
// H: 0-1, S: 0-1, L: 0-1
vec3 rgb2hsl(vec3 rgb) {
    float maxC = max(max(rgb.r, rgb.g), rgb.b);
    float minC = min(min(rgb.r, rgb.g), rgb.b);
    float l = (maxC + minC) * 0.5;

    if (maxC == minC) {
        return vec3(0.0, 0.0, l); // achromatic
    }

    float d = maxC - minC;
    float s = l > 0.5 ? d / (2.0 - maxC - minC) : d / (maxC + minC);

    float h;
    if (maxC == rgb.r) {
        h = (rgb.g - rgb.b) / d + (rgb.g < rgb.b ? 6.0 : 0.0);
    } else if (maxC == rgb.g) {
        h = (rgb.b - rgb.r) / d + 2.0;
    } else {
        h = (rgb.r - rgb.g) / d + 4.0;
    }
    h /= 6.0;

    return vec3(h, s, l);
}

float hue2rgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

// HSL to RGB conversion
vec3 hsl2rgb(vec3 hsl) {
    float h = hsl.x, s = hsl.y, l = hsl.z;

    if (s == 0.0) {
        return vec3(l); // achromatic
    }

    float q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    float p = 2.0 * l - q;

    return vec3(
        hue2rgb(p, q, h + 1.0/3.0),
        hue2rgb(p, q, h),
        hue2rgb(p, q, h - 1.0/3.0)
    );
}

// Brightness/Contrast/Gamma filter
// Matches QgsBrightnessContrastFilter::adjustColorComponent
vec3 applyBrightnessContrast(vec3 rgb) {
    // Order: center around 0.5, apply contrast, add brightness, apply gamma
    vec3 centered = rgb - 0.5;
    vec3 contrasted = centered * uContrastFactor + 0.5;
    vec3 brightened = contrasted + uBrightness / 255.0;
    vec3 gammaCorrected = pow(clamp(brightened, 0.0, 1.0), vec3(uGammaCorrection));
    return clamp(gammaCorrected, 0.0, 1.0);
}

// Hue/Saturation filter
// Matches QgsHueSaturationFilter
// Order: Invert -> Grayscale/Saturation -> Colorize
vec3 applyHueSaturation(vec3 rgb) {
    // Step 1: Invert colors
    if (uInvertColors > 0.5) {
        rgb = 1.0 - rgb;
    }

    // Step 2: Convert to HSL for color operations
    vec3 hsl = rgb2hsl(rgb);
    float h = hsl.x, s = hsl.y, l = hsl.z;

    // Step 3: Grayscale modes OR saturation scaling
    if (uGrayscaleMode > 0.5 && uGrayscaleMode < 1.5) {
        // GrayscaleLightness: use L from HSL (already have it)
        rgb = vec3(l);
        s = 0.0;
    }
    else if (uGrayscaleMode > 1.5 && uGrayscaleMode < 2.5) {
        // GrayscaleLuminosity: ITU-R BT.601 weighted RGB
        float lum = 0.21 * rgb.r + 0.72 * rgb.g + 0.07 * rgb.b;
        rgb = vec3(lum);
        hsl = rgb2hsl(rgb);
        h = hsl.x; s = hsl.y; l = hsl.z;
    }
    else if (uGrayscaleMode > 2.5) {
        // GrayscaleAverage: simple average
        float avg = (rgb.r + rgb.g + rgb.b) / 3.0;
        rgb = vec3(avg);
        hsl = rgb2hsl(rgb);
        h = hsl.x; s = hsl.y; l = hsl.z;
    }
    else if (uSaturationScale != 1.0) {
        // GrayscaleOff: apply saturation scaling
        if (uSaturationScale < 1.0) {
            // Desaturate: linear scale
            s = s * uSaturationScale;
        } else {
            // Boost: curve to prevent clipping
            // Formula: s_new = 1 - (1 - s)^(scale^2)
            float scale2 = uSaturationScale * uSaturationScale;
            s = 1.0 - pow(1.0 - s, scale2);
        }
        rgb = hsl2rgb(vec3(h, s, l));
    }

    // Step 4: Colorize (if enabled)
    if (uColorizeOn > 0.5) {
        // Replace H and S with colorize values, keep original L
        vec3 colorizedHsl = vec3(uColorizeH, uColorizeS, l);
        vec3 colorizedRgb = hsl2rgb(colorizedHsl);

        // Blend by strength
        rgb = mix(rgb, colorizedRgb, uColorizeStrength);
    }

    return rgb;
}

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

    vec3 result = stretched;

    // Apply Brightness/Contrast/Gamma filter (if active)
    if (uUseBrightnessFilter > 0.5) {
        result = applyBrightnessContrast(result);
    }

    // Apply Hue/Saturation filter (if active)
    if (uUseHueSatFilter > 0.5) {
        result = applyHueSaturation(result);
    }

    fragColor = vec4(result, uOpacity);
}
