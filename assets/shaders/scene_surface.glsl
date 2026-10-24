// The main pass's shading of one fragment, shared by its variants, which differ only in what they define first:
// triangle.frag (the main pass), triangle_surfaces.frag (the main pass writing the reflection surfaces:
// REFLECTION_SURFACES), their copies tinting shadowed lights through glass by rays (triangle_rays.frag,
// triangle_surfaces_rays.frag: GLASS_SHADOW_RAYS), the reflection probe's capture, probe_capture.frag and
// probe_capture_rays.frag (with the sun's shadow traced past its cascades and glass's tint: PROBE_CAPTURE,
// PROBE_CAPTURE_RAYS, GLASS_SHADOW_RAYS), and glass itself (triangle_transparent.frag: TRANSPARENT).

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
// How much the object scales lengths, for lengths its material gives in model units (thick glass's thickness).
layout(location = 5) flat in float objectScale;

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

// Whether this pipeline draws cut-outs (render/gpu/alpha_mask.h), throwing away their holes: a specialization constant, so
// the other pipelines hold no discard, which would keep the GPU from testing depth before shading.
#include "alpha_mask.glsl"
#include "render/gpu/material_shared.h"
layout(constant_id = MATERIAL_CUT_OUT_CONSTANT) const bool cutOut = false;


// Whether this shader draws the reflection probe's capture (render/gpu/reflection_probe.h): every light rather than
// the clusters' lists (those are the camera's view's) and no heatmap.
#include "render/gpu/reflection_probe_shared.h"
#ifdef PROBE_CAPTURE
const bool captureView = true;
#else
const bool captureView = false;
#endif

#ifdef GLASS_SHADOW_RAYS
/*
* Tinted shadows (render/gpu/transparency.h), in the variants on GPUs with ray queries used while glass is in the world:
* glass casts no shadow map, so a shadowed light's light is also passed through the glass between the point and the
* light, by a ray that sees only glass (in world space). It needs no lift off the surface: the point's own surface is
* not glass, which is not in the depth image the main pass draws over.
*/
#include "ray_scene.glsl"

vec3 glassShadow(vec3 toLight, float reach) {
    return glassTransmittance(relativePosition + cameraOrigin.xyz, toLight, reach);
}
#endif

#ifdef PROBE_CAPTURE_RAYS
// The sun's shadow by a ray where its cascades do not reach (the capture sees all round the camera, the cascades only
// what the camera looks at): from the point lifted off its surface, through the ray tracing scene (in world space).
#include "ray_scene.glsl"

// The capture finds the light mirrors reflect itself (the mirrored light pass's images are the camera's view).
#include "instances.glsl"
#include "mirrored_light_search.glsl"

float sunBeyondCascades(vec3 normal, vec3 toLight) {
    vec3 lift = normal * (float(PROBE_SHADOW_RAY_OFFSET) + float(PROBE_SHADOW_RAY_OFFSET_PER_METRE) * length(relativePosition));
    return reachesLight(relativePosition + cameraOrigin.xyz + lift, toLight, float(PROBE_SHADOW_RAY_REACH)) ? 1.0 : 0.0;
}
#else
// Past the cascades the sun counts as unshadowed, as beyond the shadow distance.
float sunBeyondCascades(vec3 normal, vec3 toLight) {
    return 1.0;
}
#endif

// Which lights may reach each cluster, one bit per light (cluster_lights.comp writes them before this pass).
layout(std430, set = 1, binding = 11) readonly buffer ClusterMasks {
    uint clusterMasks[];
};

// The shadow tiles and the atlas they are drawn into (shared with the fog's grid, fog_light.comp).
#include "scene_shadow_tiles.glsl"

// The specular albedo table (render/gpu/brdf.h): scale and bias for the reflectance head-on, over n.v and roughness.
layout(set = 1, binding = 9) uniform sampler2D brdfTable;

// The light mirrors reflect onto each pixel and the way it arrives, folded onto a square (mirrored_light.comp), read
// while mirroredLightOptions.x is 1.
layout(set = 1, binding = 22) uniform sampler2D mirroredLight;
layout(set = 1, binding = 23) uniform sampler2D mirroredDirection;

// The visibility in creases and corners (ambient_occlusion.comp), read while ambientOcclusionOptions.x is 1, and the fits
// that turn it into the occlusion of the light from all around and of the reflection.
layout(set = 1, binding = 25) uniform sampler2D ambientOcclusion;
#include "ambient_occlusion.glsl"

#ifdef TRANSPARENT
/*
* Glass and blended surfaces (render/gpu/transparency.h), drawn after the opaque scene and its reflections, sorted far to
* near, blended over the picture: the light they add, and with dual-source blending what they let through per channel
* (picture = added + transmittance x picture); without it a grey share in alpha (picture = added + (1 - alpha) x
* picture). Their reflection is the reflection probe's light in the mirror direction (its level by roughness), set 2,
* when the frame has one; the screen's ambient occlusion and mirrored lights are the opaque surface's behind, not theirs.
*/
#include "reflection_probe.glsl"
#include "transparency.glsl"
const bool transparentView = true;

// The fog in front of the glass (render/gpu/volumetric_fog.h): the picture behind it is already fogged.
#include "scene_fog.glsl"

layout(set = 2, binding = 0) uniform samplerCube transparentProbeLight;

// The picture before the glass (VulkanOpaquePicture): its colour, ever blurrier levels, and at level 0 each pixel's
// distance along the view in alpha. Refracting glass reads it.
layout(set = 2, binding = 1) uniform sampler2D opaquePicture;

// pixelToNdc, ndcToPixel and clipToScreen, for where refracted rays land on screen.
#include "screen_reflections.glsl"

// Must match GpuTransparentView (after triangle.vert's 64 bytes of push constants).
layout(push_constant) uniform TransparentView {
    layout(offset = 64) vec4 transparentProbe; // x: 1 when the reflection probe is read, y: its prefiltered levels
    vec4 transparentRefraction;                // x: 1 when the picture before the glass was kept, y: pixels per radian, z: its levels
};

/*
* What refracting glass shows of the picture behind it (render/gpu/transparency.h): thick glass bends the view ray and
* shows where it leaves (solidRefractionExit), frosted glass blurs what lies behind it, more the further behind (the
* picture's level from transmissionSpread and refractionLevel, the distance behind from the picture's alpha). Both read
* the picture from before the glass, so glass behind them is not seen.
*/
vec3 refractedPicture(vec3 normal, vec3 toCamera, float roughness, float ior, float thickness) {
    vec2 screen = clusterScreen.xy;
    vec3 exit = thickness > 0.0 ? solidRefractionExit(relativePosition, toCamera, normal, ior, thickness) : relativePosition;
    vec3 landed = clipToScreen(relativeViewProjection * vec4(exit, 1.0), screen);
    vec2 pixel = clamp(landed.xy, vec2(0.5), screen - 0.5);
    float viewDistance = dot(relativePosition, viewForward.xyz);
    float behind = max(texelFetch(opaquePicture, ivec2(pixel), 0).a - viewDistance, 0.0);
    float level = refractionLevel(transmissionSpread(roughness, ior), behind, viewDistance, transparentRefraction.y, uint(transparentRefraction.z));
    return textureLod(opaquePicture, pixel / screen, level).rgb;
}

#ifdef DUAL_SOURCE_BLENDING
layout(location = 0, index = 0) out vec4 outputColour;
layout(location = 0, index = 1) out vec4 transmittanceOutput;
#else
layout(location = 0) out vec4 outputColour;
#endif
#else
const bool transparentView = false;
layout(location = 0) out vec4 outputColour;
#endif

// The reflection surfaces for the reflection pass (render/gpu/screen_reflections.h), in their own targets: the shaded
// normal folded onto a square (octahedral, ddgi.glsl), the specular albedo with the roughness, and the stand-in for the
// reflection this pixel added (the probes' or the flat ambient's light in the mirror direction, times the specular
// albedo), which the reflection pass swaps out. Only in the variant that has those targets.
#ifdef REFLECTION_SURFACES
layout(location = 1) out vec2 reflectionNormal;
layout(location = 2) out vec4 reflectionSurface;
layout(location = 3) out vec3 reflectionStandIn;
#endif

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

            return last ? sunBeyondCascades(normal, toLight) : softShadowFromTile(nextTile, nextPosition, tanAngularRadius);
        }

        float beyond = last ? sunBeyondCascades(normal, toLight) : softShadowFromTile(nextTile, nextPosition, tanAngularRadius);
        return mix(beyond, softShadowFromTile(tile, position, tanAngularRadius), weight);
    }

    return sunBeyondCascades(normal, toLight);
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
#ifdef GLASS_SHADOW_RAYS
        // To the bulb's surface, not its centre, so glass round the bulb itself does not tint its own light.
        if (any(greaterThan(radiance, vec3(0.0)))) {
            radiance *= mix(vec3(1.0), glassShadow(toLight, max(distance - points[i].emitterRadius, 0.0)), points[i].shadowStrength);
        }
#endif
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
#ifdef GLASS_SHADOW_RAYS
        if (any(greaterThan(radiance, vec3(0.0)))) {
            radiance *= mix(vec3(1.0), glassShadow(toLight, max(distance - spots[i].emitterRadius, 0.0)), spots[i].shadowStrength);
        }
#endif
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

    // Read the textures at this fragment's interpolated UV coordinate, the mips chosen for the output's pixels rather than
    // the render size's (the bias). glTF packs roughness in green and metallic in blue.
    float mipBias = textureOptions.x;
    vec4 albedo = texture(textures[nonuniformEXT(material.textureSlots.x)], textureUv, mipBias) * material.baseColour;
    vec3 packedSurface = texture(textures[nonuniformEXT(material.textureSlots.y)], textureUv, mipBias).rgb;
    float metallic = clamp(material.surfaceParameters.x * packedSurface.b, 0.0, 1.0);
    float roughness = clamp(material.surfaceParameters.y * packedSurface.g, BRDF_SMALLEST_ROUGHNESS, 1.0);
    // The normal map is read here, outside any branch, so its mip comes from the neighbouring pixels' coordinates; bumps
    // its smaller mips averaged away widen the roughness (Toksvig).
    vec3 normalTexel = texture(textures[nonuniformEXT(material.textureSlots.z)], textureUv, mipBias).rgb;
    // The glow's pattern (white without one, so the emissive colour alone) and the baked occlusion in red (white, open,
    // without one), read here for the same reason.
    vec3 emissive = texture(textures[nonuniformEXT(material.textureSlots.w)], textureUv, mipBias).rgb * material.emissive.rgb;
    float occlusionTexel = texture(textures[nonuniformEXT(material.extra.x)], textureUv, mipBias).r;
#ifdef TRANSPARENT
    // Glass's transmission, its texture's red channel times the factor.
    float transmission = material.transmission.x * texture(textures[nonuniformEXT(material.extra.z)], textureUv, mipBias).r;
    // Thick glass's thickness texture's green channel (white without one).
    float thicknessTexel = texture(textures[nonuniformEXT(material.extra.w)], textureUv, mipBias).g;
#endif

    // A cut-out's holes are thrown away in its own pipelines (without the depth prepass; after it, the holes have no
    // depth for this pass to match). After every texture read, whose mips need the neighbouring pixels.
    if (cutOut && isCutAway(albedo.a, material.emissive.w)) {
        discard;
    }

    // The back of a double-sided surface is shaded as the front's mirror image: its normal and tangent frame turned round.
    // The capture's cube faces are mirror images, which swaps which side the GPU calls the front.
    float side = facingSign(gl_FrontFacing != captureView, (material.extra.y & MATERIAL_DOUBLE_SIDED) != 0u);
    vec3 surfaceNormal = worldNormal * side;
    vec4 surfaceTangent = worldTangent * side;

    if (textureOptions.y > 0.0) {
        roughness = toksvigRoughness(roughness, normalTexel, material.surfaceParameters.z);
    }

    // Light adds up: each light adds its share to these sums.
    vec3 diffuse = vec3(0.0);
    vec3 specular = vec3(0.0);

    float normalLength = length(surfaceNormal);
    // The camera is at the origin, so the way to it is minus the fragment's position.
    vec3 cameraOffset = -relativePosition;
    float cameraDistance = length(cameraOffset);

    // Without a usable normal there is no way to tell which side faces a light, so only ambient applies, seen head-on.
    // The geometric normal (the triangle's own, smoothed) moves points off the surface for shadow lookups; the normal
    // map's tilts are far smaller than a shadow texel and would only add noise there.
    vec3 geometricNormal = normalLength > 0.0 ? surfaceNormal / normalLength : vec3(0.0, 1.0, 0.0);
    vec3 normal = geometricNormal;

    if (normalLength > 0.0) {
        vec3 tilt = decodeNormalTexel(normalTexel, material.surfaceParameters.z);
        vec3 bumped = perturbNormal(surfaceNormal, surfaceTangent, tilt);

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
#ifdef GLASS_SHADOW_RAYS
                if (any(greaterThan(radiance, vec3(0.0)))) {
                    radiance *= glassShadow(directional[i].toLight.xyz, float(PROBE_SHADOW_RAY_REACH));
                }
#endif
            }

            addLight(radiance, directional[i].toLight.xyz, point, diffuse, specular);
        }

        if (shadingOptions.x != 0 && !captureView) {
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

        // Light reflected onto this pixel by mirrors, as one more light from where it arrives (its shadows already found
        // by the mirrored light pass's rays).
        if (mirroredLightOptions.x != 0 && !captureView && !transparentView) {
            ivec2 texel = ivec2(gl_FragCoord.xy);
            vec3 arriving = texelFetch(mirroredLight, texel, 0).rgb;

            if (any(greaterThan(arriving, vec3(0.0)))) {
                addLight(arriving, octahedralDecode(texelFetch(mirroredDirection, texel, 0).xy), point, diffuse, specular);
            }
        }

#ifdef PROBE_CAPTURE_RAYS
        // The reflection probe's traced capture runs the search itself, one footprint sample a texel (its prefiltering
        // and the frames average them), from the point lifted off its surface.
        if (mirrorPairCounts.x > 0) {
            vec3 lift = geometricNormal * (float(PROBE_SHADOW_RAY_OFFSET) + float(PROBE_SHADOW_RAY_OFFSET_PER_METRE) * length(relativePosition));
            MirroredLight mirrored = mirroredLightAt(relativePosition, relativePosition + lift, gl_FragCoord.xy, MIRROR_HIT_FOOTPRINT_SAMPLES);

            if (any(greaterThan(mirrored.total, vec3(0.0)))) {
                addLight(mirrored.total, mirroredLightDirection(mirrored), point, diffuse, specular);
            }
        }
#endif
    }

    // The light from all around: the probes' bounced light where they reach (their visibility tested along the
    // geometric normal, steadier than the normal map's; the light read along the shaded normal, and in the mirror
    // direction as the specular part's stand-in until reflections exist), the flat ambient elsewhere; the still and
    // fast lights' atlases added. Without probes, the flat ambient alone.
    vec3 ambientReflected = shadeAmbient(point, flatAmbient());
    vec3 mirrorLight = flatAmbient();
    vec3 diffuseLight = flatAmbient();

    if (probesOn) {
        vec3 reflected = reflect(-toCamera, shadingNormal);
        ProbeLights probeLights = probeAmbient(relativePosition, geometricNormal, shadingNormal, reflected, toCamera, 0, true);
        mirrorLight = probeLights.still.specular + probeLights.fast.specular;
        diffuseLight = probeLights.still.diffuse + probeLights.fast.diffuse;
        ambientReflected = shadeProbeAmbient(point, diffuseLight, mirrorLight);
    }

#ifdef TRANSPARENT
    // Glass has no screen-space reflection (it is not in the depth image), so its reflection is the probe's light.
    if (transparentProbe.x > 0.5) {
        mirrorLight = textureLod(transparentProbeLight, reflect(-toCamera, shadingNormal), probeRoughnessLevel(roughness, transparentProbe.y)).rgb;
        ambientReflected = shadeProbeAmbient(point, diffuseLight, mirrorLight);
    }
#endif

    // Ambient occlusion dims the light from all around in creases and corners (not the direct light, which has its
    // shadows): the diffuse part by the visibility, raised by the light a crease's walls bounce back in (more for bright
    // ones), the reflection's stand-in by the specular occlusion, which the reflection pass's better light keeps (it is
    // in the specular albedo the reflection surfaces hand on). The screen's occlusion is not in the reflection probe's
    // capture, which has none of its own view; a material's baked occlusion is, being the surface's own. The two count
    // as whichever is more closed. Without either the light is left exactly as it was.
    float reflectionOcclusion = 1.0;
    bool screenOcclusion = ambientOcclusionOptions.x != 0 && !captureView && !transparentView;

    if (screenOcclusion || material.surfaceParameters.w > 0.0) {
        float screenVisibility = screenOcclusion ? texelFetch(ambientOcclusion, ivec2(gl_FragCoord.xy), 0).r : 1.0;
        float visibility = surfaceVisibility(screenVisibility, occlusionTexel, material.surfaceParameters.w);
        vec3 diffuseOcclusion = ambientOcclusionOptions.y != 0 ? multiBounceOcclusion(visibility, matteColour(albedo.rgb, metallic)) : vec3(visibility);
        reflectionOcclusion = ambientOcclusionOptions.z != 0 ? specularOcclusion(point.nDotV, visibility, roughness) : 1.0;
        mirrorLight *= reflectionOcclusion;
        ambientReflected = shadeProbeAmbient(point, diffuseLight * diffuseOcclusion, mirrorLight);
    }

#ifdef TRANSPARENT
    // The diffuse and specular parts apart (the light from all around split as shadeProbeAmbient adds it), for glass to
    // swap its diffuse part for what is behind. Coverage only for blended materials; an opaque one's alpha is ignored.
    vec3 ambientSpecular = mirrorLight * point.specularAlbedo;
    float coverage = (material.extra.y & MATERIAL_BLEND) != 0u ? albedo.a : 1.0;
    TransparentLight seen = transparentBlend(coverage, transmission, metallic, ambientReflected - ambientSpecular + diffuse, ambientSpecular + specular,
                                             emissive, albedo.rgb, point.specularAlbedo);

    // Refracting glass (thick or frosted) adds what it lets through of the picture before the glass, where its view
    // ray lands and as blurred as its roughness, absorbed on the way through its thickness; only its uncovered share
    // lets the picture through as it is.
    if ((material.extra.y & MATERIAL_REFRACTS) != 0u && transparentRefraction.x > 0.5) {
        float thickness = material.transmission.y * thicknessTexel * objectScale;
        vec3 through = transparentTransmittance(1.0, transmission, metallic, albedo.rgb, point.specularAlbedo) *
                       beerAttenuation(material.attenuation.rgb, material.transmission.z, thickness);
        seen.added += coverage * through * refractedPicture(shadingNormal, toCamera, roughness, material.transmission.w, thickness);
        seen.transmittance = vec3(1.0 - coverage);
    }

    // Seen through the fog in front of it: its light dimmed, and the fog's light for the share it does not let through
    // (the picture it lets through already holds the rest).
    if (fogOn()) {
        seen.added = fogOverGlass(seen.added, seen.transmittance, fogAlongView(relativePosition, false));
    }
    outputColour = vec4(seen.added, 1.0 - (seen.transmittance.r + seen.transmittance.g + seen.transmittance.b) / 3.0);
#ifdef DUAL_SOURCE_BLENDING
    transmittanceOutput = vec4(seen.transmittance, 1.0);
#endif
#else
    // Emitted light is added as is.
    outputColour = vec4(ambientReflected + diffuse + specular + emissive, albedo.a);
#endif

    // A surface without a normal reflects nothing sharply: fully rough, so the stand-in stays.
#ifdef REFLECTION_SURFACES
    reflectionNormal = octahedralEncode(shadingNormal);
    reflectionSurface = vec4(point.specularAlbedo * reflectionOcclusion, normalLength > 0.0 ? roughness : 1.0);
    reflectionStandIn = mirrorLight * point.specularAlbedo;
#endif

    // The heatmap debug view: the picture mostly covered by the colour of how many lights this pixel's cluster lists.
    if (shadingOptions.y != 0 && !captureView) {
        outputColour.rgb = mix(outputColour.rgb, clusterHeatColour(clusterLightCount()), 0.75);
    }
}
