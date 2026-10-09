#version 450

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 worldPosition;

layout(set = 2, binding = 0) uniform sampler2D colourTexture;

// The point light shadow atlas: six depth images (cube faces) in a 3x2 grid. A shadow sampler compares a depth we give it
// with the stored one and returns 1 (lit) or 0 (blocked), blended across neighbouring texels for slightly soft edges.
layout(set = 2, binding = 1) uniform sampler2DShadow pointShadowAtlas;

// Must match MaterialUniformData in render/gpu/material_uniforms.h.
layout(std140, set = 3, binding = 0) uniform MaterialData {
    vec4 baseColour;
    vec4 specularParameters; // x: strength, y: shininess
    vec4 emissive;           // rgb: light the surface gives off itself
};

// Must match the sizes and layout of LightUniformData in render/gpu/light_uniforms.h.
struct DirectionalLightData {
    vec4 toLight;  // xyz: unit direction towards the light
    vec4 radiance; // rgb: colour times intensity
};

struct PointLightData {
    vec4 positionRange; // xyz: world position, w: range
    vec4 radiance;      // rgb: colour times intensity, w: radius of the glowing source
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
    ivec4 counts;        // x: directional lights used, y: point lights used, z: spotlights used, w: shadowed point light or -1
    DirectionalLightData directional[4];
    PointLightData points[16];
    SpotLightData spots[4];
};

// Must match ShadowUniformData in render/gpu/point_shadow.h.
layout(std140, set = 3, binding = 2) uniform ShadowData {
    mat4 faceMatrices[6]; // each cube face's view-projection, the same ones the shadow pass drew with
    vec4 shadowSettings;  // x: normal offset per unit of distance, y: atlas texel width, z: atlas texel height
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

/*
* The face whose camera sees this direction from the light: the axis of the largest component, in the order
* +X, -X, +Y, -Y, +Z, -Z. Must match pointShadowFace in render/gpu/point_shadow.cpp.
*/
int pointShadowFace(vec3 fromLight) {
    vec3 size = abs(fromLight);

    if (size.x >= size.y && size.x >= size.z) {
        return fromLight.x > 0.0 ? 0 : 1;
    }

    if (size.y >= size.z) {
        return fromLight.y > 0.0 ? 2 : 3;
    }

    return fromLight.z > 0.0 ? 4 : 5;
}

/*
* How much of the shadowed point light reaches this point: 1 lit, 0 blocked, in between at a shadow's soft edge.
* The point is first nudged off its surface along the normal, by about a shadow texel, so a surface does not shadow itself
* (shadow acne). Then it is projected exactly as the shadow pass drew the scene, and its depth is compared with what the
* light saw there. Nine nearby comparisons are averaged (percentage-closer filtering) so edges are not jagged.
*/
float pointShadow(vec3 lightPosition, vec3 normal) {
    float distanceToLight = length(worldPosition - lightPosition);
    vec3 position = worldPosition + normal * shadowSettings.x * distanceToLight;

    int face = pointShadowFace(position - lightPosition);
    vec4 clip = faceMatrices[face] * vec4(position, 1.0);
    vec3 projected = clip.xyz / clip.w;

    // From -1..1 across the face to 0..1 within it (texture rows run top to bottom), then to the face's tile in the atlas.
    vec2 withinFace = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    vec2 tile = vec2(face % 3, face / 3);
    vec2 atlasUv = (tile + withinFace) / vec2(3.0, 2.0);

    float lit = 0.0;

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(x, y) * shadowSettings.yz;
            lit += texture(pointShadowAtlas, vec3(atlasUv + offset, projected.z));
        }
    }

    return lit / 9.0;
}

void main() {
    // Read the texture at this fragment's interpolated UV coordinate.
    vec4 albedo = texture(colourTexture, textureUv) * baseColour;

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

            vec3 radiance = points[i].radiance.rgb * distanceFalloff(distance, range, points[i].radiance.w);

            if (i == counts.w) {
                radiance *= pointShadow(points[i].positionRange.xyz, normal);
            }

            addLight(radiance, offset / distance, normal, toCamera, diffuse, specular);
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

    // The surface colour tints scattered light; the highlight keeps the light's own colour; emitted light is added as is.
    outputColour = vec4(albedo.rgb * (ambient.rgb + diffuse) + specular + emissive.rgb, albedo.a);
}
