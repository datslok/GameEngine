#version 450

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 worldPosition;

layout(set = 2, binding = 0) uniform sampler2D colourTexture;

layout(std140, set = 3, binding = 0) uniform MaterialData {
    vec4 baseColour;
};

// Must match the sizes and layout of LightUniformData in render/gpu/light_uniforms.h.
struct DirectionalLightData {
    vec4 toLight;  // xyz: unit direction towards the light
    vec4 radiance; // rgb: colour times intensity
};

struct PointLightData {
    vec4 positionRange; // xyz: world position, w: range
    vec4 radiance;
};

layout(std140, set = 3, binding = 1) uniform LightData {
    vec4 ambient;
    ivec4 counts; // x: directional lights used, y: point lights used
    DirectionalLightData directional[4];
    PointLightData points[16];
};

layout(location = 0) out vec4 outputColour;

void main() {
    // Read the texture at this fragment's interpolated UV coordinate.
    vec4 albedo = texture(colourTexture, textureUv) * baseColour;

    // Light adds up: start with the ambient fill and add each light's share.
    vec3 light = ambient.rgb;

    float normalLength = length(worldNormal);

    // Without a usable normal there is no way to tell which side faces a light, so only ambient applies.
    if (normalLength > 0.0) {
        vec3 normal = worldNormal / normalLength;

        for (int i = 0; i < counts.x; ++i) {
            light += directional[i].radiance.rgb * max(dot(normal, directional[i].toLight.xyz), 0.0);
        }

        for (int i = 0; i < counts.y; ++i) {
            vec3 offset = points[i].positionRange.xyz - worldPosition;
            float range = points[i].positionRange.w;
            float distance = length(offset);

            if (distance <= 0.0) {
                continue;
            }

            // Inverse square falloff (+1 keeps it finite at the light), times a window that reaches exactly 0 at range with no visible edge.
            float ratio = distance / range;
            float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
            float attenuation = window * window / (distance * distance + 1.0);

            light += points[i].radiance.rgb * max(dot(normal, offset / distance), 0.0) * attenuation;
        }
    }

    outputColour = vec4(albedo.rgb * light, albedo.a);
}
