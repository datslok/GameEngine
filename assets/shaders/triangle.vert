#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 colour;

layout(location = 0) out vec3 vertexColour;

layout(std140, set = 1, binding = 0) uniform TransformData {
    mat4 transform;
};

void main() {
    gl_Position = transform * vec4(position, 1.0);
    vertexColour = colour;
}