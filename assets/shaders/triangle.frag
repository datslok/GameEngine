#version 450

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
layout(location = 2) in vec3 worldPosition;

layout(set = 2, binding = 0) uniform sampler2D colourTexture;

// The shadow atlas: every light's depth images in square tiles of 256 to 1024 texels, packed into one texture. A shadow sampler compares a depth we give it
// with the stored one and returns 1 (lit) or 0 (blocked), blended across neighbouring texels for slightly soft edges.
layout(set = 2, binding = 1) uniform sampler2DShadow shadowAtlas;

// The same atlas read as plain numbers, the stored depths themselves, which soft shadows need to find how far away the
// things blocking a light are (a comparison only says whether something is in the way).
layout(set = 2, binding = 2) uniform sampler2D shadowDepths;

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
// can read only 4 KB of one), so they are in a storage buffer: set 2, after the three samplers.
struct ShadowTileData {
    mat4 matrix; // the tile's view-projection, the same one the shadow pass drew with
    vec4 offset; // normal offset against acne: x per unit of distance from the light, y fixed
    vec4 rect;   // where the tile is in the atlas, 0..1: xy corner, zw size
    vec4 lens;   // perspective tiles only (else zeros): x near, y far plane, z world size of a texel per unit of distance
};

layout(std430, set = 2, binding = 3) readonly buffer ShadowTiles {
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
    vec4 pointEmitterRadii[16];    // the size of point light i's glowing part, at [i / 4][i % 4], for soft shadows
    vec4 spotEmitterRadii[2];
};

layout(location = 0) out vec4 outputColour;

// How the surface position changes from this pixel to the next one right and down. Soft shadows use it to follow the
// surface's slope across their wide filter. Set at the start of main, where every pixel of a 2x2 block runs together
// (screen-space derivatives are only defined there, not inside the per-light loops some pixels leave early).
vec3 positionStepRight;
vec3 positionStepDown;

float interleavedGradientNoise(vec2 pixel);

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

// Soft shadows: the arithmetic mirrors render/gpu/soft_shadow.cpp, which tests it. Widths are in texels of the tile.
const float minShadowFilterTexels = 1.0;
const float maxShadowFilterTexels = 12.0;
const float maxBlockerSearchTexels = 12.0;

// Sixteen points spread over a unit disc with no two close together (a Poisson disc): even coverage from few samples.
const vec2 poissonDisc[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
);

float linearShadowDepth(float depth, float nearPlane, float farPlane) {
    return farPlane * nearPlane / (farPlane - depth * (farPlane - nearPlane));
}

float blockerSearchTexels(float emitterRadius, float receiverDepth, float nearPlane, float texelPerUnit) {
    float widthPerDistance = emitterRadius * (receiverDepth - nearPlane) / (nearPlane * receiverDepth);
    return clamp(widthPerDistance / texelPerUnit, minShadowFilterTexels, maxBlockerSearchTexels);
}

float penumbraTexels(float emitterRadius, float receiverDepth, float blockerDepth, float texelPerUnit) {
    float worldWidth = emitterRadius * max(receiverDepth - blockerDepth, 0.0) / blockerDepth;
    return clamp(worldWidth / (receiverDepth * texelPerUnit), minShadowFilterTexels, maxShadowFilterTexels);
}

// Where a world point lands in the atlas (xy) and its stored-depth value (z), for one tile.
vec3 atlasPoint(int tile, vec3 position) {
    vec4 clip = shadowTiles[tile].matrix * vec4(position, 1.0);
    vec3 projected = clip.xyz / clip.w;
    vec2 withinTile = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    return vec3(shadowTiles[tile].rect.xy + withinTile * shadowTiles[tile].rect.zw, projected.z);
}

/*
* Percentage-closer soft shadows (PCSS) for a perspective tile: how much of a glowing area of radius emitterRadius the
* point sees. Directional boxes, and lights without an emitter size, use the plain 3x3 filter.
* 1. Blocker search: look around the point for stored depths nearer the light than it, and average their distances.
* 2. Penumbra: from that, how wide the soft edge is here (wider the further the point is behind the blocker).
* 3. Filter: average sixteen depth comparisons spread over that width.
* A wide filter on a sloping surface would compare the surface with itself further along, where it is nearer to or
* further from the light, and shadow itself. So each sample compares against the surface's own plane at that spot
* (receiver plane depth bias), worked out from how the point moves in the tile from one screen pixel to the next.
* The sample pattern is turned by a different angle in every pixel, which trades banding for fine grain.
*/
float softShadowFromTile(int tile, vec3 position, float emitterRadius) {
    if (shadowTiles[tile].rect.z <= 0.0) {
        return 1.0;
    }

    vec4 lens = shadowTiles[tile].lens;

    if (lens.y <= 0.0 || emitterRadius <= 0.0) {
        return shadowFromTile(tile, position);
    }

    vec4 clip = shadowTiles[tile].matrix * vec4(position, 1.0);
    vec3 projected = clip.xyz / clip.w;

    if (clip.w <= 0.0 || any(greaterThan(abs(projected.xy), vec2(1.0))) || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    vec3 here = atlasPoint(tile, position);

    // The surface's plane in the tile: stored-depth change per step in atlas coordinates, from two neighbouring pixels.
    vec3 towardsRight = atlasPoint(tile, position + positionStepRight) - here;
    vec3 towardsDown = atlasPoint(tile, position + positionStepDown) - here;
    mat2 atlasSteps = mat2(towardsRight.xy, towardsDown.xy);
    vec2 depthSlope = vec2(0.0);

    if (abs(determinant(atlasSteps)) > 1e-14) {
        depthSlope = inverse(transpose(atlasSteps)) * vec2(towardsRight.z, towardsDown.z);
    }

    // Samples stay a texel inside the tile, so they never read a neighbouring light's tile.
    vec2 lowest = shadowTiles[tile].rect.xy + atlasTexel.xy;
    vec2 highest = shadowTiles[tile].rect.xy + shadowTiles[tile].rect.zw - atlasTexel.xy;

    float angle = 6.2831853 * interleavedGradientNoise(gl_FragCoord.xy);
    mat2 turn = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));

    float receiverDistance = linearShadowDepth(here.z, lens.x, lens.y);
    float searchRadius = blockerSearchTexels(emitterRadius, receiverDistance, lens.x, lens.z);
    float blockerDistances = 0.0;
    float blockers = 0.0;

    for (int i = 0; i < 16; ++i) {
        vec2 uv = clamp(here.xy + turn * poissonDisc[i] * searchRadius * atlasTexel.xy, lowest, highest);
        float stored = textureLod(shadowDepths, uv, 0.0).r;

        if (stored < here.z + dot(depthSlope, uv - here.xy)) {
            blockerDistances += linearShadowDepth(stored, lens.x, lens.y);
            blockers += 1.0;
        }
    }

    // Nothing in the way of any part of the emitter.
    if (blockers == 0.0) {
        return 1.0;
    }

    float filterRadius = penumbraTexels(emitterRadius, receiverDistance, blockerDistances / blockers, lens.z);
    float lit = 0.0;

    for (int i = 0; i < 16; ++i) {
        vec2 uv = clamp(here.xy + turn * poissonDisc[i] * filterRadius * atlasTexel.xy, lowest, highest);
        lit += texture(shadowAtlas, vec3(uv, here.z + dot(depthSlope, uv - here.xy)));
    }

    return lit / 16.0;
}

// A point light has six tiles in a row, one per cube face; the face is picked from the direction to the point.
float pointLightShadow(int firstTile, vec3 lightPosition, vec3 normal, float emitterRadius) {
    vec3 position = offsetForShadow(firstTile, normal, length(worldPosition - lightPosition));
    return softShadowFromTile(firstTile + pointShadowFace(position - lightPosition), position, emitterRadius);
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
    positionStepRight = dFdx(worldPosition);
    positionStepDown = dFdy(worldPosition);

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

            // Beyond its range a light gives exactly nothing, so skip it before the costly part, its shadow lookups.
            if (distance <= 0.0 || distance >= range) {
                continue;
            }

            vec3 radiance = points[i].radiance.rgb * distanceFalloff(distance, range, points[i].radiance.w);

            int firstTile = pointTiles[i / 4][i % 4];

            if (firstTile >= 0) {
                radiance *= mix(1.0, pointLightShadow(firstTile, points[i].positionRange.xyz, normal, pointEmitterRadii[i / 4][i % 4]), pointShadowStrengths[i / 4][i % 4]);
            }

            addLight(radiance, offset / distance, normal, toCamera, diffuse, specular);
        }

        for (int i = 0; i < counts.z; ++i) {
            vec3 offset = spots[i].positionRange.xyz - worldPosition;
            float distance = length(offset);

            if (distance <= 0.0 || distance >= spots[i].positionRange.w) {
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
                radiance *= mix(1.0, softShadowFromTile(tile, offsetForShadow(tile, normal, distance), spotEmitterRadii[i / 4][i % 4]), spotShadowStrengths[i / 4][i % 4]);
            }
            addLight(radiance, toLight, normal, toCamera, diffuse, specular);
        }
    }

    // The surface colour tints scattered light; the highlight keeps the light's own colour; emitted light is added as is.
    outputColour = vec4(ditherForEightBits(albedo.rgb * (ambient.rgb + diffuse) + specular + emissive.rgb), albedo.a);
}
