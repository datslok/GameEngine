#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec2 textureUv;

layout(std140, set = 1, binding = 0) uniform TransformData {
    mat4 transform;
    mat4 model;
};

void main() {
    gl_Position = transform * vec4(position, 1.0);

    // The GPU interpolates UVs across the triangle.
    textureUv = uv;

    mat3 modelLinear = mat3(model);
    worldNormal = vec3(0.0);

    // Transform normals correctly under non-uniform scaling.
    if (determinant(modelLinear) != 0.0) {
        mat3 normalMatrix = transpose(inverse(modelLinear));
        vec3 transformedNormal = normalMatrix * normal;

        float normalLength = length(transformedNormal);

        if (normalLength > 0.0) {
            worldNormal = transformedNormal / normalLength;
        }
    }
}