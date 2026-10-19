#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_KHR_shader_subgroup_arithmetic : require

// Numbers shared with the C++ side, written once (the makefile passes -Isrc to glslc).
#include "render/gpu/shader_shared.h"

// The shading maths, in pure functions shared with the GPU tests (tests/gpu) and mirrored in C++ (render/gpu).
#include "brdf.glsl"
#include "normal_mapping.glsl"
#include "lighting.glsl"
#include "soft_shadow.glsl"
#include "cascades.glsl"
#include "clusters.glsl"

// Per-pixel noise, which turns each pixel's soft shadow samples.
#include "dither.glsl"

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec2 textureUv;
// Where the fragment is relative to the camera (camera-relative rendering): world axes, the camera at the origin. Light
// positions and shadow matrices are measured the same way.
layout(location = 2) in vec3 relativePosition;
// The surface's tangent (direction of increasing u) and in w the side of it the normal map's "up" is on.
layout(location = 3) in vec4 worldTangent;
// Which entry of the material buffer shades this object (from its instance; the same across the triangle).
layout(location = 4) flat in uint materialIndex;

// Every texture (bindless) and the frame's materials.
#include "materials.glsl"

// The frame's uniform block: ambient light, light counts and the atlas texel size are what this shader reads of it.
#include "frame_data.glsl"
#include "frame_clusters.glsl"

// The frame's lights (shared with cluster_lights.comp).
#include "scene_lights.glsl"

// The light probes' bounced light (global illumination), when the frame has probes.
#include "scene_probes.glsl"

// Whether this pipeline samples the probes: a specialization constant, so the pipelines without probes hold none of their
// code (with it merely skipped by a branch, the driver compiled the rest a little differently, and a few pixels moved).
layout(constant_id = DDGI_SAMPLE_PROBES_CONSTANT) const bool probesOn = false;

// Which lights may reach each cluster, one bit per light (cluster_lights.comp writes them before this pass).
layout(std430, set = 1, binding = 11) readonly buffer ClusterMasks {
    uint clusterMasks[];
};

// Must match ShadowTileData in render/gpu/shadow_map.h.
struct ShadowTileData {
    mat4 matrix; // the shadow pass's view-projection, for points measured from the camera
    vec4 offset; // against acne: x per unit of distance from the light, y fixed (a sun cascade's: its base bias towards the sun)
    vec4 rect;   // where the tile is in the atlas, 0..1: xy corner, zw size
    // Perspective tiles: x near, y far plane, z world size of a texel per unit of distance. A sun's cascade boxes: x 0
    // (no near plane), y the box's depth, z one texel's world size, w the cascade's radius.
    vec4 lens;
};

layout(std430, set = 1, binding = 6) readonly buffer ShadowTiles {
    ShadowTileData shadowTiles[];
};

// The shadow atlas: every light's depth images in square tiles of 256 to 2048 texels, packed into one texture. A shadow sampler compares a depth we give it
// with the stored one and returns 1 (lit) or 0 (blocked), blended across neighbouring texels for slightly soft edges.
layout(set = 1, binding = 7) uniform sampler2DShadow shadowAtlas;

// The same atlas read as plain numbers, the stored depths themselves, which soft shadows need to find how far away the
// things blocking a light are (a comparison only says whether something is in the way).
layout(set = 1, binding = 8) uniform sampler2D shadowDepths;

// The specular albedo table (render/gpu/brdf.h): scale and bias for the reflectance head-on, over n.v and roughness.
layout(set = 1, binding = 9) uniform sampler2D brdfTable;

layout(location = 0) out vec4 outputColour;

// How the surface position changes from this pixel to the next one right and down. Soft shadows use it to follow the
// surface's slope across their wide filter. Set at the start of main, where every pixel of a 2x2 block runs together
// (screen-space derivatives are only defined there, not inside the per-light loops some pixels leave early).
vec3 positionStepRight;
vec3 positionStepDown;

// Texel i holds the value at i / (size - 1); a linear sampler places it at (i + 0.5) / size.
vec2 sampleBrdfTable(float nDotV, float roughness) {
    vec2 grid = vec2(nDotV, roughness) * float(BRDF_TABLE_SIZE - 1);
    return textureLod(brdfTable, (grid + 0.5) / float(BRDF_TABLE_SIZE), 0.0).rg;
}

/*
* The point moved off its surface along the normal by about a shadow texel, so a surface does not shadow itself (shadow acne).
* Texels cover more of the world further from a light in a perspective view, hence the part that grows with distance.
*/
vec3 offsetForShadow(int tile, vec3 normal, float distanceToLight) {
    return relativePosition + normal * (shadowTiles[tile].offset.x * distanceToLight + shadowTiles[tile].offset.y);
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

// Sixteen points spread over a unit disc with no two close together (a Poisson disc): even coverage from few samples.
const vec2 poissonDisc[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
);

// Where a point (measured from the camera) lands in the atlas (xy) and its stored-depth value (z), for one tile.
vec3 atlasPoint(int tile, vec3 position) {
    vec4 clip = shadowTiles[tile].matrix * vec4(position, 1.0);
    vec3 projected = clip.xyz / clip.w;
    vec2 withinTile = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    return vec3(shadowTiles[tile].rect.xy + withinTile * shadowTiles[tile].rect.zw, projected.z);
}

/*
* Percentage-closer soft shadows (PCSS): how much of a glowing area the point sees. For a perspective tile the area is a
* bulb of radius emitterSize, distances are measured from the light and the penumbra spreads like a cone from it; for a
* sun's cascade box (no near plane) emitterSize is tan of the sun's angular radius, distances are measured from the box's
* near side and the penumbra widens by the same angle at any distance. Lights without a size use the plain 3x3 filter.
* 1. Blocker search: look around the point for stored depths nearer the light than it (larger, with reversed-Z), and average their distances.
* 2. Penumbra: from that, how wide the soft edge is here (wider the further the point is behind the blocker).
* 3. Filter: average sixteen depth comparisons spread over that width.
* A wide filter on a sloping surface would compare the surface with itself further along, where it is nearer to or
* further from the light, and shadow itself. So each sample compares against the surface's own plane at that spot
* (receiver plane depth bias), worked out from how the point moves in the tile from one screen pixel to the next.
* The sample pattern is turned by a different angle in every pixel, which trades banding for fine grain.
*/
float softShadowFromTile(int tile, vec3 position, float emitterSize) {
    if (shadowTiles[tile].rect.z <= 0.0) {
        return 1.0;
    }

    vec4 lens = shadowTiles[tile].lens;
    bool box = lens.x <= 0.0;

    if (lens.y <= 0.0 || emitterSize <= 0.0) {
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

    float receiverDistance = box ? boxShadowDistance(here.z, lens.y) : linearShadowDepth(here.z, lens.x, lens.y);
    float searchRadius = box ? sunBlockerSearchTexels(emitterSize, receiverDistance, lens.z)
                             : blockerSearchTexels(emitterSize, receiverDistance, lens.x, lens.z);
    float blockerDistances = 0.0;
    float blockers = 0.0;

    for (int i = 0; i < 16; ++i) {
        vec2 uv = clamp(here.xy + turn * poissonDisc[i] * searchRadius * atlasTexel.xy, lowest, highest);
        float stored = textureLod(shadowDepths, uv, 0.0).r;

        if (stored > here.z + dot(depthSlope, uv - here.xy)) {
            blockerDistances += box ? boxShadowDistance(stored, lens.y) : linearShadowDepth(stored, lens.x, lens.y);
            blockers += 1.0;
        }
    }

    // Nothing in the way of any part of the emitter.
    if (blockers == 0.0) {
        return 1.0;
    }

    float blockerDistance = blockerDistances / blockers;
    float filterRadius = box ? sunPenumbraTexels(emitterSize, receiverDistance, blockerDistance, lens.z)
                             : penumbraTexels(emitterSize, receiverDistance, blockerDistance, lens.z);
    float lit = 0.0;

    for (int i = 0; i < 16; ++i) {
        vec2 uv = clamp(here.xy + turn * poissonDisc[i] * filterRadius * atlasTexel.xy, lowest, highest);
        lit += texture(shadowAtlas, vec3(uv, here.z + dot(depthSlope, uv - here.xy)));
    }

    return lit / 16.0;
}

// How far a point is inside a sun cascade's box, in units of its radius (see cascadeEdgeDistance): negative outside.
float sunCascadeEdge(int tile, vec3 position) {
    vec4 clip = shadowTiles[tile].matrix * vec4(position, 1.0);
    vec4 lens = shadowTiles[tile].lens;
    return cascadeEdgeDistance(clip.xyz / clip.w, lens.y / lens.w);
}

/*
* The sun's shadow from its cascades (render/gpu/cascades.h), tiles in a row from firstTile, finest first. The point is
* shadowed by the finest cascade whose box holds it, each moving the point towards the sun by its own bias (sunShadowBias:
* along the rays, so every cascade still finds the shadow's edges in the same place). Within
* the blend band near that box's edge, the next cascade takes over gradually: both are looked up and mixed, or with the
* dithered switch each pixel takes one of them, the next more often nearer the edge. Past the last cascade's edge the
* shadow fades to lit, and a point no box holds is lit.
*/
float sunShadow(int firstTile, int cascades, vec3 normal, vec3 toLight, float tanAngularRadius) {
    float band = cascadeOptions.x;
    bool dither = cascadeOptions.y != 0.0;
    float nDotL = dot(normal, toLight);

    for (int cascade = 0; cascade < cascades; ++cascade) {
        int tile = firstTile + cascade;
        vec3 position = relativePosition + toLight * sunShadowBias(shadowTiles[tile].offset.y, nDotL);
        float edge = sunCascadeEdge(tile, position);

        if (edge <= 0.0) {
            continue;
        }

        float weight = cascadeBlendWeight(edge, band);

        if (weight >= 1.0) {
            return softShadowFromTile(tile, position, tanAngularRadius);
        }

        // In the band: the next cascade's share comes from it, or past the last one from plain light. A point the next
        // box misses (only at its corners) stays wholly with this one.
        bool last = cascade + 1 == cascades;
        int nextTile = tile + 1;
        vec3 nextPosition = position;

        if (!last) {
            nextPosition = relativePosition + toLight * sunShadowBias(shadowTiles[nextTile].offset.y, nDotL);

            if (sunCascadeEdge(nextTile, nextPosition) <= 0.0) {
                return softShadowFromTile(tile, position, tanAngularRadius);
            }
        }

        if (dither) {
            // Noise of its own, offset from the soft shadow's, so the choice does not follow the samples' turn.
            if (cascadeDitherKeeps(weight, interleavedGradientNoise(gl_FragCoord.xy + vec2(23.0, 41.0)))) {
                return softShadowFromTile(tile, position, tanAngularRadius);
            }

            return last ? 1.0 : softShadowFromTile(nextTile, nextPosition, tanAngularRadius);
        }

        float beyond = last ? 1.0 : softShadowFromTile(nextTile, nextPosition, tanAngularRadius);
        return mix(beyond, softShadowFromTile(tile, position, tanAngularRadius), weight);
    }

    return 1.0;
}

// A point light has six tiles in a row, one per cube face; the face is picked from the direction to the point.
float pointLightShadow(int firstTile, vec3 lightPosition, vec3 normal, float emitterRadius) {
    vec3 position = offsetForShadow(firstTile, normal, length(relativePosition - lightPosition));
    return softShadowFromTile(firstTile + pointShadowFace(position - lightPosition), position, emitterRadius);
}

/*
* One point light's share at this pixel, added to the sums. Beyond its range a light gives exactly nothing, so it is
* skipped before the costly part, its shadow lookups; so is a light behind the surface.
*/
void addPointLight(int i, ShadingPoint point, vec3 geometricNormal, inout vec3 diffuse, inout vec3 specular) {
    vec3 offset = points[i].positionRange.xyz - relativePosition;
    float range = points[i].positionRange.w;
    float distance = length(offset);

    if (distance <= 0.0 || distance >= range) {
        return;
    }

    vec3 toLight = offset / distance;

    if (!facesLight(point, toLight)) {
        return;
    }

    vec3 radiance = points[i].radiance.rgb * distanceFalloff(distance, range, points[i].radiance.w);

    int firstTile = points[i].firstShadowTile;

    if (firstTile >= 0) {
        radiance *= mix(1.0, pointLightShadow(firstTile, points[i].positionRange.xyz, geometricNormal, points[i].emitterRadius), points[i].shadowStrength);
    }

    addLight(radiance, toLight, point, diffuse, specular);
}

// One spotlight's share at this pixel: as a point light, and nothing outside its cone.
void addSpotLight(int i, ShadingPoint point, vec3 geometricNormal, inout vec3 diffuse, inout vec3 specular) {
    vec3 offset = spots[i].positionRange.xyz - relativePosition;
    float distance = length(offset);

    if (distance <= 0.0 || distance >= spots[i].positionRange.w) {
        return;
    }

    vec3 toLight = offset / distance;

    // How close this fragment is to the beam's axis, as a cosine.
    float alongBeam = dot(-toLight, spots[i].directionCosOuter.xyz);
    float cone = spotCone(alongBeam, spots[i].directionCosOuter.w, spots[i].radianceCosInner.w);

    if (cone <= 0.0 || !facesLight(point, toLight)) {
        return;
    }

    vec3 radiance = spots[i].radianceCosInner.rgb * distanceFalloff(distance, spots[i].positionRange.w, spots[i].sourceRadius) * cone;
    int tile = spots[i].shadowTile;

    if (tile >= 0) {
        radiance *= mix(1.0, softShadowFromTile(tile, offsetForShadow(tile, geometricNormal, distance), spots[i].emitterRadius), spots[i].shadowStrength);
    }
    addLight(radiance, toLight, point, diffuse, specular);
}

/*
* The lights of a whole wave (the group of pixels the GPU shades in lockstep, 32 on NVIDIA): the union of every
* pixel's cluster list. Neighbouring pixels in different clusters would otherwise walk different lists, and the wave
* would pay for each in turn; walking the union together, every pixel reads the same light records at the same time.
* A pixel gets nothing from a light that is not in its own list (it cannot reach the pixel), so the sums are unchanged.
*/
uint lightsOfTheWave(uint bits) {
    return subgroupOr(bits);
}

// Where this pixel's cluster's bits start in the cluster masks.
int firstClusterWord() {
    return clusterOfPixel(frameClusterGrid(), gl_FragCoord.xy, dot(relativePosition, viewForward.xyz)) * CLUSTER_WORDS;
}

// How many lights this pixel's cluster lists, for the heatmap debug view.
int clusterLightCount() {
    int firstWord = firstClusterWord();
    int lights = 0;

    for (int word = 0; word * 32 < counts.y; ++word) {
        lights += bitCount(clusterMasks[firstWord + word]);
    }

    for (int word = 0; word * 32 < counts.z; ++word) {
        lights += bitCount(clusterMasks[firstWord + CLUSTER_POINT_WORDS + word]);
    }

    return lights;
}

void main() {
    positionStepRight = dFdx(relativePosition);
    positionStepDown = dFdy(relativePosition);

    MaterialData material = materials[materialIndex];

    // Read the textures at this fragment's interpolated UV coordinate. glTF packs roughness in green and metallic in blue.
    vec4 albedo = texture(textures[nonuniformEXT(material.textureSlots.x)], textureUv) * material.baseColour;
    vec3 packedSurface = texture(textures[nonuniformEXT(material.textureSlots.y)], textureUv).rgb;
    float metallic = clamp(material.surfaceParameters.x * packedSurface.b, 0.0, 1.0);
    float roughness = clamp(material.surfaceParameters.y * packedSurface.g, BRDF_SMALLEST_ROUGHNESS, 1.0);

    // Light adds up: each light adds its share to these sums.
    vec3 diffuse = vec3(0.0);
    vec3 specular = vec3(0.0);

    float normalLength = length(worldNormal);
    // The camera is at the origin, so the way to it is minus the fragment's position.
    vec3 cameraOffset = -relativePosition;
    float cameraDistance = length(cameraOffset);

    // Without a usable normal there is no way to tell which side faces a light, so only ambient applies, seen head-on.
    // The geometric normal (the triangle's own, smoothed) moves points off the surface for shadow lookups; the normal
    // map's tilts are far smaller than a shadow texel and would only add noise there.
    vec3 geometricNormal = normalLength > 0.0 ? worldNormal / normalLength : vec3(0.0, 1.0, 0.0);
    vec3 normal = geometricNormal;

    if (normalLength > 0.0) {
        vec3 tilt = decodeNormalTexel(texture(textures[nonuniformEXT(material.textureSlots.z)], textureUv).rgb, material.surfaceParameters.z);
        vec3 bumped = perturbNormal(worldNormal, worldTangent, tilt);

        if (dot(bumped, bumped) > 0.0) {
            normal = bumped;
        }
    }

    vec3 toCamera = cameraDistance > 0.0 ? cameraOffset / cameraDistance : normal;
    vec3 shadingNormal = normalLength > 0.0 ? normal : toCamera;
    vec2 tableEntry = sampleBrdfTable(viewCosine(shadingNormal, toCamera), roughness);
    ShadingPoint point = prepareShading(albedo.rgb, metallic, roughness, shadingNormal, toCamera, tableEntry);

    if (normalLength > 0.0) {
        for (int i = 0; i < counts.x; ++i) {
            // A light from behind gives nothing, so its shadow lookups would be wasted.
            if (!facesLight(point, directional[i].toLight.xyz)) {
                continue;
            }

            vec3 radiance = directional[i].radiance.rgb;
            int tile = directional[i].shadowTile;

            if (tile >= 0) {
                radiance *= sunShadow(tile, directional[i].cascadeCount, geometricNormal, directional[i].toLight.xyz, directional[i].tanAngularRadius);
            }

            addLight(radiance, directional[i].toLight.xyz, point, diffuse, specular);
        }

        if (shadingOptions.x != 0) {
            // Only the lights listed for this pixel's cluster (cluster_lights.comp), lowest bit first, which is slot
            // order: every light that reaches this point is listed, so these are the lights the loops below would add,
            // in the same order, and the picture is the same to the last bit.
            int firstWord = firstClusterWord();

            for (int word = 0; word * 32 < counts.y; ++word) {
                uint bits = lightsOfTheWave(clusterMasks[firstWord + word]);

                while (bits != 0u) {
                    int bit = findLSB(bits);
                    bits &= bits - 1u;
                    addPointLight(word * 32 + bit, point, geometricNormal, diffuse, specular);
                }
            }

            for (int word = 0; word * 32 < counts.z; ++word) {
                uint bits = lightsOfTheWave(clusterMasks[firstWord + CLUSTER_POINT_WORDS + word]);

                while (bits != 0u) {
                    int bit = findLSB(bits);
                    bits &= bits - 1u;
                    addSpotLight(word * 32 + bit, point, geometricNormal, diffuse, specular);
                }
            }
        } else {
            for (int i = 0; i < counts.y; ++i) {
                addPointLight(i, point, geometricNormal, diffuse, specular);
            }

            for (int i = 0; i < counts.z; ++i) {
                addSpotLight(i, point, geometricNormal, diffuse, specular);
            }
        }
    }

    // The light from all around: the probes' bounced light where they reach (their visibility tested along the
    // geometric normal, steadier than the normal map's; the light read along the shaded normal, and in the mirror
    // direction as the specular part's stand-in until reflections exist), the flat ambient elsewhere. Without probes,
    // the flat ambient alone.
    vec3 ambientReflected = shadeAmbient(point, ambient.rgb);

    if (probesOn) {
        vec3 reflected = reflect(-toCamera, shadingNormal);
        ProbeLight probeLight = probeAmbient(relativePosition, geometricNormal, shadingNormal, reflected, toCamera, probeOrigin.xyz, true);
        ambientReflected = shadeProbeAmbient(point, probeLight.diffuse, probeLight.specular);
    }

    // Emitted light is added as is.
    outputColour = vec4(ambientReflected + diffuse + specular + material.emissive.rgb, albedo.a);

    // The heatmap debug view: the picture mostly covered by the colour of how many lights this pixel's cluster lists.
    if (shadingOptions.y != 0) {
        outputColour.rgb = mix(outputColour.rgb, clusterHeatColour(clusterLightCount()), 0.75);
    }
}
