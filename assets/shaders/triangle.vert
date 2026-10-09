#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec2 textureUv;
layout(location = 2) out vec3 worldPosition;

layout(std140, set = 1, binding = 0) uniform TransformData {
    mat4 transform;
    mat4 model;
    mat4 normalMatrix; // Inverse transpose of the model's 3x3 part, computed once per object on the CPU (zero if the object is squashed flat).
};

void main() {
    gl_Position = transform * vec4(position, 1.0);

    // Point lights need to know where each fragment is in the world.
    worldPosition = (model * vec4(position, 1.0)).xyz;

    // The GPU interpolates UVs across the triangle.
    textureUv = uv;

    // The normal matrix keeps normals perpendicular to the surface under non-uniform scaling.
    vec3 transformedNormal = mat3(normalMatrix) * normal;
    float normalLength = length(transformedNormal);

    worldNormal = normalLength > 0.0 ? transformedNormal / normalLength : vec3(0.0);
}
