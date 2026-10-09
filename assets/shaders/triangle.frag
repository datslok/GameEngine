#version 450

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 worldPosition;

layout(set = 2, binding = 0) uniform sampler2D colourTexture;

// Must match MaterialUniformData in render/gpu/material_uniforms.h.
layout(std140, set = 3, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 specularParameters; // x: strength, y: shininess, z: 1 for an unlit material (temporary, see Material::unlit)
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

struct SpotLightData {
    vec4 positionRange;     // xyz: world position, w: range
    vec4 directionCosOuter; // xyz: unit beam direction, w: cosine of the outer cone angle
    vec4 radianceCosInner;  // rgb: colour times intensity, w: cosine of the inner cone angle
    vec4 sourceRadius;      // x: radius of the glowing source, for the falloff
};

layout(std140, set = 3, binding = 1) uniform LightData {
    vec4 ambient;
    vec4 cameraPosition; // xyz: where the viewer is
    ivec4 counts;        // x: directional lights used, y: point lights used, z: spotlights used
    DirectionalLightData directional[4];
    PointLightData points[16];
    SpotLightData spots[4];
};

layout(location = 0) out vec4 outputColour;

/*
* One light's contribution, already scaled by distance falloff (and the cone, for spotlights).
* Diffuse (Lambert) scatters evenly, so it does not depend on the viewer. Specular (Blinn-Phong) is the mirror-like part:
* it is brightest when the halfway vector between the light and the viewer lines up with the surface normal.
*/
void addLight(vec3 radiance, vec3 toLight, vec3 normal, vec3 toCamera, inout vec3 diffuse, inout vec3 specular) {
    float facing = dot(normal, toLight);

    // No light, and so no highlight, on the side facing away.
    if (facing <= 0.0) {
        return;
    }

    diffuse += radiance * facing;

    vec3 halfway = toLight + toCamera;
    float halfwayLength = length(halfway);

    if (halfwayLength > 0.0) {
        float alignment = max(dot(normal, halfway / halfwayLength), 0.0);
        float shininess = max(specularParameters.y, 1.0);
        specular += radiance * specularParameters.x * pow(alignment, shininess);
    }
}

/*
* Inverse square falloff, softened near the light by the size of its source (a point light uses 1, which keeps it finite at the light),
* times a window that reaches exactly 0 at range with no visible edge.
*/
float distanceFalloff(float distance, float range, float sourceRadius) {
    float ratio = distance / range;
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / (distance * distance + sourceRadius * sourceRadius);
}

void main() {
    // Read the texture at this fragment's interpolated UV coordinate.
    vec4 albedo = texture(colourTexture, textureUv) * baseColour;

    // TEMPORARY: an unlit material shows its colour as is (the demo's MOBA marker, until it becomes a HUD element).
    if (specularParameters.z > 0.5) {
        outputColour = albedo;
        return;
    }

    // Light adds up: each light adds its share to these sums.
    vec3 diffuse = vec3(0.0);
    vec3 specular = vec3(0.0);

    float normalLength = length(worldNormal);
    vec3 cameraOffset = cameraPosition.xyz - worldPosition;
    float cameraDistance = length(cameraOffset);

    // Without a usable normal there is no way to tell which side faces a light, so only ambient applies.
    if (normalLength > 0.0) {
        vec3 normal = worldNormal / normalLength;
        vec3 toCamera = cameraDistance > 0.0 ? cameraOffset / cameraDistance : normal;

        for (int i = 0; i < counts.x; ++i) {
            addLight(directional[i].radiance.rgb, directional[i].toLight.xyz, normal, toCamera, diffuse, specular);
        }

        for (int i = 0; i < counts.y; ++i) {
            vec3 offset = points[i].positionRange.xyz - worldPosition;
            float range = points[i].positionRange.w;
            float distance = length(offset);

            if (distance <= 0.0) {
                continue;
            }

            addLight(points[i].radiance.rgb * distanceFalloff(distance, range, 1.0), offset / distance, normal, toCamera, diffuse, specular);
        }

        for (int i = 0; i < counts.z; ++i) {
            vec3 offset = spots[i].positionRange.xyz - worldPosition;
            float distance = length(offset);

            if (distance <= 0.0) {
                continue;
            }

            vec3 toLight = offset / distance;

            // How close this fragment is to the beam's axis, as a cosine: full inside the inner cone,
            // fading smoothly to nothing at the outer cone, like a real flashlight's soft edge.
            float alongBeam = dot(-toLight, spots[i].directionCosOuter.xyz);
            float cone = smoothstep(spots[i].directionCosOuter.w, spots[i].radianceCosInner.w, alongBeam);

            if (cone <= 0.0) {
                continue;
            }

            vec3 radiance = spots[i].radianceCosInner.rgb * distanceFalloff(distance, spots[i].positionRange.w, spots[i].sourceRadius.x) * cone;
            addLight(radiance, toLight, normal, toCamera, diffuse, specular);
        }
    }

    // The surface colour tints scattered light; the highlight keeps the light's own colour.
    outputColour = vec4(albedo.rgb * (ambient.rgb + diffuse) + specular, albedo.a);
}
