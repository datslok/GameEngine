#version 450

// The shadow pass only needs where each vertex is, as seen from one face of the light's cube.
layout(location = 0) in vec3 position;

layout(std140, set = 1, binding = 0) uniform ShadowTransform {
    mat4 transform; // the face's view-projection times the model matrix
};

void main() {
    gl_Position = transform * vec4(position, 1.0);
}