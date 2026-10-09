#version 450

layout(location = 0) in vec3 lineColour;

layout(location = 0) out vec4 outputColour;

// Unlit: a debug line shows its colour as it is, whatever the lights do.
void main() {
    outputColour = vec4(lineColour, 1.0);
}