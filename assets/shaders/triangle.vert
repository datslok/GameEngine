#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
// xyz: the surface's direction of increasing u; w: +1 or -1, which side of it "up the picture" is (MeshVertex).
layout(location = 3) in vec4 tangent;

layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec2 textureUv;
layout(location = 2) out vec3 relativePosition;
layout(location = 3) out vec4 worldTangent;
// Which entry of the material buffer shades this object: the same for every fragment of a triangle (flat, not interpolated).
layout(location = 4) flat out uint materialIndex;

// One object of the GPU scene (render/gpu/gpu_scene.h, GpuInstance). Camera-relative rendering (render/gpu/camera_relative.h):
// positions are measured from the camera along the world's axes, so the numbers stay small and precise however far from
// the world's origin the camera is.
struct InstanceData {
    mat4 transform;    // the camera-relative view-projection times the camera-relative model matrix
    mat4 model;        // places the vertex relative to the camera
    mat4 normalMatrix; // Inverse transpose of the model's 3x3 part, computed once per object on the CPU (zero if the object is squashed flat).
    vec4 handedness;   // x: -1 if the model matrix mirrors space, else +1
    uvec4 material;    // x: the material's number in the material buffer
};

// Every object drawn this frame, in batch order: an instanced draw's gl_InstanceIndex counts from its batch's first one.
layout(std430, set = 1, binding = 1) readonly buffer Instances {
    InstanceData instances[];
};

void main() {
    mat4 transform = instances[gl_InstanceIndex].transform;
    mat4 model = instances[gl_InstanceIndex].model;
    mat4 normalMatrix = instances[gl_InstanceIndex].normalMatrix;
    vec4 handedness = instances[gl_InstanceIndex].handedness;
    materialIndex = instances[gl_InstanceIndex].material.x;

    gl_Position = transform * vec4(position, 1.0);

    // Point lights need to know where each fragment is, relative to the camera like the lights themselves.
    relativePosition = (model * vec4(position, 1.0)).xyz;

    // The GPU interpolates UVs across the triangle.
    textureUv = uv;

    // The normal matrix keeps normals perpendicular to the surface under non-uniform scaling.
    vec3 transformedNormal = mat3(normalMatrix) * normal;
    float normalLength = length(transformedNormal);

    worldNormal = normalLength > 0.0 ? transformedNormal / normalLength : vec3(0.0);

    // Tangents lie in the surface like edges, so the model matrix itself carries them. On a mirrored object the bitangent
    // rebuilt from normal and tangent would come out reversed, so the sign takes the matrix's handedness. Mirrored and
    // tested as transformTangent in render/gpu/normal_mapping.cpp.
    vec3 transformedTangent = mat3(model) * tangent.xyz;
    float tangentLength = length(transformedTangent);

    worldTangent = vec4(tangentLength > 0.0 ? transformedTangent / tangentLength : vec3(0.0), tangent.w * handedness.x);
}
