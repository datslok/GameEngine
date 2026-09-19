#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

// Every vertex of a triangle carries the same face normal.
layout(location = 0) flat out vec3 worldNormal;

layout(std140, set = 1, binding = 0) uniform TransformData {
    mat4 transform;
    mat4 model;
};

void main() {
    gl_Position = transform * vec4(position, 1.0);

    mat3 modelLinear = mat3(model);

    // The inverse transpose handles rotation and non-uniform scaling.
    // A zero scale makes the matrix singular, so use a zero normal.
    worldNormal = vec3(0.0);

    if (determinant(modelLinear) != 0.0) {
        mat3 normalMatrix = transpose(inverse(modelLinear));
        worldNormal = normalMatrix * normal;
    }
}