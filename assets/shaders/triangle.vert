#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame_data.glsl"
#include "instances.glsl"

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

// The depth prepass runs this same shader, and the main pass then shades only fragments whose depth EQUALS the one the
// prepass stored. Invariant makes the compiler compute the position the same way in both pipelines, bit for bit (it
// may not, for instance, fuse a multiply and an add in one and not the other).
invariant gl_Position;

void main() {
    InstanceData instance = instances[drawList[gl_InstanceIndex]];
    mat4 normalMatrix = instance.normalMatrix;
    vec4 handedness = instance.handedness;
    materialIndex = instance.material.x;

    // Camera-relative rendering (render/gpu/camera_relative.h): the camera is subtracted from the model matrix's
    // translation before any product, as relativeTo does, so positions are measured from the camera along the world's
    // axes and the numbers stay small and precise however far from the world's origin the camera is. Point lights
    // need to know where each fragment is, relative to the camera like the lights themselves.
    mat4 model = instance.model;
    model[3].xyz -= cameraOrigin.xyz;
    relativePosition = (model * vec4(position, 1.0)).xyz;
    gl_Position = relativeViewProjection * vec4(relativePosition, 1.0);

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
