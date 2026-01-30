#version 450

// Vertex shader for GPU raster tile rendering
// Transforms tile vertices from map coordinates to clip space

layout(location = 0) in vec2 aPosition;  // Tile corner in map units
layout(location = 1) in vec2 aTexCoord;  // Texture coordinates (0-1)

layout(location = 0) out vec2 vTexCoord;

layout(std140, binding = 0) uniform Transform {
    mat4 uMVPMatrix;
};

void main() {
    gl_Position = uMVPMatrix * vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
