#version 450

layout(location = 0) in vec3 worldNormal;

layout(std140, set = 3, binding = 0) uniform MaterialData {
    vec4 baseColour;
};

layout(location = 0) out vec4 outputColour;

void main() {
    // World-space direction from the surface toward the light.
    vec3 toLight = normalize(vec3(-1.0, 2.0, 1.0));

    float ambient = 0.2;
    float diffuse = 0.0;

    float normalLength = length(worldNormal);

    if (normalLength > 0.0) {
        vec3 normal = worldNormal / normalLength;
        diffuse = clamp(dot(normal, toLight), 0.0, 1.0);
    }

    float brightness = ambient + (1.0 - ambient) * diffuse;

    outputColour = vec4(
        baseColour.rgb * brightness,
        baseColour.a
    );
}