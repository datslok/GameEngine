#version 450

// Debug lines: each vertex has a world position and a colour, and the camera's view-projection places it.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 colour;

layout(location = 0) out vec3 lineColour;

layout(std140, set = 1, binding = 0) uniform DebugLineData {
    mat4 viewProjection;
};

void main() {
    gl_Position = viewProjection * vec4(position, 1.0);
    lineColour = colour;
}