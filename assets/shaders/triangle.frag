#version 450

layout(std140, set = 3, binding = 0) uniform MaterialData {
    vec4 baseColour;
};

layout(location = 0) out vec4 outputColour;

void main() {
    outputColour = baseColour;
}