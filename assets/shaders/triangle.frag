#version 450

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 worldPosition;

layout(set = 2, binding = 0) uniform sampler2D colourTexture;

// The shadow atlas: every light's depth images in square tiles of 256 to 1024 texels, packed into one texture. A shadow sampler compares a depth we give it
// with the stored one and returns 1 (lit) or 0 (blocked), blended across neighbouring texels for slightly soft edges.
layout(set = 2, binding = 1) uniform sampler2DShadow shadowAtlas;

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
    ivec4 counts;        // x: directional lights used, y: point lights used, z: spotlights used
    DirectionalLightData directional[4];
    PointLightData points[64];
    SpotLightData spots[8];
};

// Must match ShadowTileData in render/gpu/shadow_map.h. Over a hundred tiles do not fit in a uniform block (the shader
// can read only 4 KB of one), so they are in a storage buffer: set 2, after the two samplers.
struct ShadowTileData {
    mat4 matrix; // the tile's view-projection, the same one the shadow pass drew with
    vec4 offset; // normal offset against acne: x per unit of distance from the light, y fixed
    vec4 rect;   // where the tile is in the atlas, 0..1: xy corner, zw size
};

layout(std430, set = 2, binding = 2) readonly buffer ShadowTiles {
    ShadowTileData shadowTiles[];
};

// Must match ShadowUniformData in render/gpu/shadow_map.h. Tile numbers are -1 for a light without a shadow.
layout(std140, set = 3, binding = 2) uniform ShadowData {
    ivec4 pointTiles[16];  // first of six tiles for point light i at [i / 4][i % 4]
    ivec4 spotTiles[2];    // tile for spotlight i at [i / 4][i % 4]
    ivec4 directionalTiles;
    vec4 atlasTexel;       // xy: one texel's size in atlas coordinates
    vec4 pointShadowStrengths[16]; // how strong point light i's shadow is, 0..1, at [i / 4][i % 4] (fades between lights)
    vec4 spotShadowStrengths[2];
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
* +X, -X, +Y, -Y, +Z, -Z. Must match pointShadowFace in render/gpu/shadow_map.cpp.
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
* The point moved off its surface along the normal by about a shadow texel, so a surface does not shadow itself (shadow acne).
* Texels cover more of the world further from a light in a perspective view, hence the part that grows with distance.
*/
vec3 offsetForShadow(int tile, vec3 normal, float distanceToLight) {
    return worldPosition + normal * (shadowTiles[tile].offset.x * distanceToLight + shadowTiles[tile].offset.y);
}

/*
* How much light reaches a point according to one tile: 1 lit, 0 blocked, in between at a shadow's soft edge.
* The point is projected exactly as the shadow pass drew the scene, and its depth is compared with what the light saw
* there. Nine nearby comparisons are averaged (percentage-closer filtering) so edges are not jagged.
* Points outside the tile's view (beyond a directional light's shadow box) count as lit.
*/
float shadowFromTile(int tile, vec3 position) {
    // A cube face the camera cannot see got no square in the atlas (an empty rectangle).
    if (shadowTiles[tile].rect.z <= 0.0) {
        return 1.0;
    }

    vec4 clip = shadowTiles[tile].matrix * vec4(position, 1.0);
    vec3 projected = clip.xyz / clip.w;

    if (clip.w <= 0.0 || any(greaterThan(abs(projected.xy), vec2(1.0))) || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    // From -1..1 across the tile to 0..1 within it (texture rows run top to bottom), then to the tile's place in the atlas.
    vec2 withinTile = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    vec2 atlasUv = shadowTiles[tile].rect.xy + withinTile * shadowTiles[tile].rect.zw;

    float lit = 0.0;

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            lit += texture(shadowAtlas, vec3(atlasUv + vec2(x, y) * atlasTexel.xy, projected.z));
        }
    }

    return lit / 9.0;
}

// A point light has six tiles in a row, one per cube face; the face is picked from the direction to the point.
float pointLightShadow(int firstTile, vec3 lightPosition, vec3 normal) {
    vec3 position = offsetForShadow(firstTile, normal, length(worldPosition - lightPosition));
    return shadowFromTile(firstTile + pointShadowFace(position - lightPosition), position);
}

// The sRGB curve, as in core/srgb.cpp.
vec3 linearToSrgb(vec3 linear) {
    return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), linear));
}

vec3 srgbToLinear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

/*
* Dithering against banding. The screen keeps 256 levels per channel, and a slow, dark fade (the flashlight's pool at night)
* changes by less than one level over many pixels, so it rounds into visible rings. Adding a little noise first, up to half
* a level either way and different per pixel, makes nearby pixels round to the two levels around the true value in the
* right proportion, and the eye averages them: the rings become fine grain. Mirrors core/dither.cpp, which tests the maths.
*/
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

vec3 ditherForEightBits(vec3 linear) {
    vec3 encoded = linearToSrgb(max(linear, vec3(0.0)));
    float noise = interleavedGradientNoise(gl_FragCoord.xy);
    vec3 nudged = encoded + (noise - 0.5) * (0.99 / 255.0);
    return srgbToLinear(clamp(nudged, 0.0, 1.0));
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
            vec3 radiance = directional[i].radiance.rgb;
            int tile = directionalTiles[i];

            if (tile >= 0) {
                radiance *= shadowFromTile(tile, offsetForShadow(tile, normal, 0.0));
            }

            addLight(radiance, directional[i].toLight.xyz, normal, toCamera, diffuse, specular);
        }

        for (int i = 0; i < counts.y; ++i) {
            vec3 offset = points[i].positionRange.xyz - worldPosition;
            float range = points[i].positionRange.w;
            float distance = length(offset);

            if (distance <= 0.0) {
                continue;
            }

            vec3 radiance = points[i].radiance.rgb * distanceFalloff(distance, range, points[i].radiance.w);

            int firstTile = pointTiles[i / 4][i % 4];

            if (firstTile >= 0) {
                radiance *= mix(1.0, pointLightShadow(firstTile, points[i].positionRange.xyz, normal), pointShadowStrengths[i / 4][i % 4]);
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
            int tile = spotTiles[i / 4][i % 4];

            if (tile >= 0) {
                radiance *= mix(1.0, shadowFromTile(tile, offsetForShadow(tile, normal, distance)), spotShadowStrengths[i / 4][i % 4]);
            }
            addLight(radiance, toLight, normal, toCamera, diffuse, specular);
        }
    }

    // The surface colour tints scattered light; the highlight keeps the light's own colour; emitted light is added as is.
    outputColour = vec4(ditherForEightBits(albedo.rgb * (ambient.rgb + diffuse) + specular + emissive.rgb), albedo.a);
}
