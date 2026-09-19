#version 450

layout(location = 0) in vec3 vertexColour;
layout(location = 0) out vec4 outputColour;

void main() {
    outputColour = vec4(vertexColour, 1.0);
}