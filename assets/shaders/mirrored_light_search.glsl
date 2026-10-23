// The mirrored light search (render/gpu/mirrored_light.h), with its rays: the light reaching a point by way of one
// reflection off each pair of a fast light and a shiny surface the CPU chose this frame. Shared by the mirrored light
// pass (mirrored_light.comp, each pixel of the view), the rays' hits (ray_hit_lighting.glsl: the light probes' rays,
// so the lit spot bounces, and traced reflections) and the reflection probe's traced capture (scene_surface.glsl). The
// loop solveMirrorPath and footprintLight mirror; the maths is in mirrored_light.glsl. Included after ray_scene.glsl,
// materials.glsl, instances.glsl, frame_data.glsl and scene_lights.glsl. Everything is measured from the camera; the
// rays go through the world.

#ifndef MIRRORED_LIGHT_SEARCH_GLSL
#define MIRRORED_LIGHT_SEARCH_GLSL

#include "brdf.glsl"
#include "ddgi.glsl"
#include "dither.glsl"
#include "mirrored_light.glsl"

// Must match GpuMirrorPair and GpuMirrorPairs.
struct MirrorPair {
    ivec4 light;  // x: the light's kind, y: its slot, z: the draw
    vec4 bounds;  // xyz: the draw's bounding sphere's centre, measured from the camera, w: its radius
};

layout(std430, set = 1, binding = 24) readonly buffer MirrorPairs {
    ivec4 mirrorPairCounts;  // x: the pairs (none when mirrored lights are off)
    vec4 mirrorPairOptions;  // x: the setting's roughness limit
    MirrorPair mirrorPairs[MIRROR_MAX_PAIRS];
};

// How far a ray from the receiver looks for the mirror (metres): past anything a light's range reaches.
const float mirrorRayReach = 10000.0;

// Where a ray met the mirror, for its material once the search is done.
struct MirrorHit {
    MirrorSurface surface;
    int primitive;
    vec2 barycentrics;
};

/*
* The surface where a ray from start towards target first meets something solid: false unless that is the pair's draw,
* met from the front with its smoothed normal facing the ray. The surface's curvature comes from the triangle's corners.
*/
bool projectOntoMirror(vec3 start, vec3 target, int draw, out MirrorHit hit) {
    vec3 direction = normalize(target - start);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene, gl_RayFlagsOpaqueEXT, RAY_MASK_SOLID, start + cameraOrigin.xyz, 0.0, direction, mirrorRayReach);

    while (rayQueryProceedEXT(query)) {
    }

    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT ||
        rayQueryGetIntersectionInstanceCustomIndexEXT(query, true) != draw || !rayQueryGetIntersectionFrontFaceEXT(query, true)) {
        return false;
    }

    hit.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
    InstanceData instance = instances[draw];
    MeshTriangle corners = meshTriangleAt(instance.material.y, uint(hit.primitive));
    vec3 weights = hitCornerWeights(hit.barycentrics.x, hit.barycentrics.y);
    mat3 model = mat3(instance.model);
    mat3 normalMatrix = mat3(instance.normalMatrix);
    vec3 normalSum = normalMatrix * (corners.normals[0] * weights.x + corners.normals[1] * weights.y + corners.normals[2] * weights.z);
    float normalLength = length(normalSum);

    if (!(normalLength > 0.0) || dot(normalSum, direction) >= 0.0) {
        return false;
    }

    hit.surface.position = start + direction * rayQueryGetIntersectionTEXT(query, true);
    hit.surface.frame = tangentFrame(normalSum / normalLength);
    hit.surface.curvature = triangleCurvature(model * (corners.positions[1] - corners.positions[0]), model * (corners.positions[2] - corners.positions[0]),
                                              normalMatrix * (corners.normals[1] - corners.normals[0]),
                                              normalMatrix * (corners.normals[2] - corners.normals[0]), normalLength, hit.surface.frame);
    return true;
}

/*
* The search, as solveMirrorPath: the seed's ray, the flat mirror step (on a flat surface, a point off the mirror on its
* plane if it falls off: onMirror false, the hit's material still the seed's), then Newton steps halved while they fall
* off.
*/
bool solveMirrorPath(vec3 receiver, vec3 start, vec3 light, vec3 seed, int draw, out MirrorHit hit, out bool onMirror) {
    onMirror = true;

    if (!projectOntoMirror(start, seed, draw, hit)) {
        return false;
    }

    vec3 image;
    MirrorHit moved;
    bool imaged = planeMirrorPoint(receiver, light, hit.surface.position, hit.surface.frame.normal, image);
    bool flatSurface = isFlatMirror(hit.surface.curvature);

    // On a flat mirror the step is exact, so landing anywhere but its own plane (a thin mirror's rim) is landing off it.
    if (imaged && projectOntoMirror(start, image, draw, moved) &&
        (!flatSurface || dot(moved.surface.frame.normal, hit.surface.frame.normal) >= float(MIRROR_SAME_PLANE_COSINE))) {
        hit = moved;
    } else if (flatSurface) {
        // Off the mirror, unless the receiver is on its plane (the mirror itself, which its reflection cannot light).
        onMirror = false;
        bool offPlane = dot(receiver - hit.surface.position, hit.surface.frame.normal) > float(MIRROR_RAY_OFFSET);
        hit.surface.position = image;
        return imaged && offPlane;
    }

    for (int steps = 0;; ++steps) {
        if (length(mirrorConstraint(receiver, light, hit.surface)) <= float(MIRROR_SOLVE_TOLERANCE)) {
            return mirrorIncidence(receiver, hit.surface) > 0.0 && dot(light - hit.surface.position, hit.surface.frame.normal) > 0.0;
        }

        vec3 target;

        if (steps == MIRROR_MAX_STEPS || !mirrorNewtonTarget(receiver, light, hit.surface, target)) {
            return false;
        }

        MirrorHit next;
        bool landed = projectOntoMirror(start, target, draw, next);

        for (int halving = 0; halving < MIRROR_BACKTRACKS && !landed; ++halving) {
            target = (hit.surface.position + target) * 0.5;
            landed = projectOntoMirror(start, target, draw, next);
        }

        if (!landed) {
            return false;
        }

        hit = next;
    }
}

// What a pair's light sends: its position, range, intensity towards a point, the falloff's source radius and the size
// of its glowing part (for a rough mirror's footprint).
struct PairLight {
    vec3 position;
    float range;
    vec3 colour;
    float sourceRadius;
    float emitterRadius;
    bool spot;
    vec3 beam;
    float cosOuter;
    float cosInner;
};

PairLight pairLight(ivec4 light) {
    if (light.x == MIRROR_SPOT_LIGHT) {
        SpotLightData spot = spots[light.y];
        return PairLight(spot.positionRange.xyz, spot.positionRange.w, spot.radianceCosInner.rgb, spot.sourceRadius, spot.emitterRadius, true,
                         spot.directionCosOuter.xyz, spot.directionCosOuter.w, spot.radianceCosInner.w);
    }

    PointLightData point = points[light.y];
    return PairLight(point.positionRange.xyz, point.positionRange.w, point.radiance.rgb, point.radiance.w, point.emitterRadius, false, vec3(0.0), -1.0,
                     -1.0);
}

// How much of the light reaches a point on the mirror: a spotlight's cone there, then a shadow ray (none with the light
// behind the surface).
float lightReachesMirror(PairLight light, MirrorSurface surface) {
    vec3 point = surface.position;
    vec3 normal = surface.frame.normal;
    vec3 toLight = light.position - point;
    float lightDistance = length(toLight);
    float cone = light.spot ? spotCone(dot(-toLight / lightDistance, light.beam), light.cosOuter, light.cosInner) : 1.0;

    if (cone <= 0.0 || dot(toLight, normal) <= 0.0) {
        return 0.0;
    }

    vec3 shadowFrom = point + normal * (float(MIRROR_RAY_OFFSET) + float(MIRROR_RAY_OFFSET_PER_METRE) * length(point));
    vec3 toLightFromShadow = light.position - shadowFrom;
    float shadowDistance = length(toLightFromShadow);
    return reachesLight(shadowFrom + cameraOrigin.xyz, toLightFromShadow / shadowDistance, shadowDistance) ? cone : 0.0;
}

// The light arriving by way of the mirrors: its sum, and the ways it arrives weighted by its brightness.
struct MirroredLight {
    vec3 total;
    vec3 weightedDirection;
};

/*
* The light reflected onto receiver by every pair, its rays leaving from start (the receiver lifted off its surface on
* the side the light can come from). A rough mirror's footprint takes samples points each frame, turned by noise from
* noisePixel (different for neighbouring pixels or rays) and the frame, which temporal anti-aliasing or the caller's
* own averaging smooths.
*/
MirroredLight mirroredLightAt(vec3 receiver, vec3 start, vec2 noisePixel, int samples) {
    MirroredLight light = MirroredLight(vec3(0.0), vec3(0.0));
    vec2 framePixel = noisePixel + 5.588238 * float(mirroredLightOptions.y);
    float turn = interleavedGradientNoise(framePixel);
    float stratum = interleavedGradientNoise(framePixel + vec2(23.0, 41.0));

    for (int i = 0; i < mirrorPairCounts.x; ++i) {
        MirrorPair pair = mirrorPairs[i];
        PairLight source = pairLight(pair.light);

        if (!mirrorPairMayReach(receiver, pair.bounds.xyz, pair.bounds.w, source.position, source.range)) {
            continue;
        }

        MirrorHit hit;
        bool onMirror;

        if (!solveMirrorPath(receiver, start, source.position, pair.bounds.xyz, pair.light.z, hit, onMirror)) {
            continue;
        }

        // The mirror's reflectance at this angle, from its material as the main pass would shade it, faded out past the
        // setting's roughness.
        InstanceData instance = instances[pair.light.z];
        MaterialData material = materials[instance.material.x];
        MeshSurface surface = meshSurfaceAt(instance.material.y, uint(hit.primitive), hit.barycentrics);
        vec3 baseColour = textureLod(textures[nonuniformEXT(material.textureSlots.x)], surface.uv, 0.0).rgb * material.baseColour.rgb;
        vec3 packedSurface = textureLod(textures[nonuniformEXT(material.textureSlots.y)], surface.uv, 0.0).rgb;
        float metallic = clamp(material.surfaceParameters.x * packedSurface.b, 0.0, 1.0);
        float roughness = clamp(material.surfaceParameters.y * packedSurface.g, 0.0, 1.0);
        float fade = mirrorRoughnessFade(roughness, mirrorPairOptions.x);

        if (fade <= 0.0) {
            continue;
        }

        float incidence = mirrorIncidence(receiver, hit.surface);
        vec3 reflectance = schlickFresnel(mix(vec3(BRDF_DIELECTRIC_REFLECTANCE), baseColour, metallic), incidence) * fade;
        vec3 point = hit.surface.position;
        float receiverDistance = length(receiver - point);
        vec3 arriving;

        if (usesMirrorFootprint(roughness)) {
            // The footprint's samples, as footprintLight: each a ray proving it on the mirror and in the light.
            MirrorFootprint footprint = mirrorFootprint(receiver, source.position, hit.surface, roughness * roughness, source.emitterRadius, pair.bounds.w);
            float sum = 0.0;

            for (int index = 0; index < samples; ++index) {
                vec2 offset = footprintOffset(footprint, gaussianSamplePoint(index, samples, turn, stratum));
                MirrorHit landed;

                if (!projectOntoMirror(start, point + hit.surface.frame.tangent * offset.x + hit.surface.frame.bitangent * offset.y, pair.light.z, landed)) {
                    continue;
                }

                float lit = lightReachesMirror(source, landed.surface);

                if (lit > 0.0) {
                    sum += footprintWeight(footprint, offset) * lit;
                }
            }

            arriving = roughMirroredIrradiance(source.colour, reflectance, receiver, source.position, hit.surface, sum / float(samples), source.range);
        } else {
            float lit = onMirror ? lightReachesMirror(source, hit.surface) : 0.0;

            if (lit <= 0.0) {
                continue;
            }

            arriving = mirroredIrradiance(source.colour * lit, reflectance, mirrorSpreadArea(receiver, source.position, hit.surface),
                                          receiverDistance + length(source.position - point), source.range, source.sourceRadius);
        }

        light.total += arriving;
        light.weightedDirection += (point - receiver) * (probeLuminance(arriving) / receiverDistance);
    }

    return light;
}

// The way the light arrives (any way where there is none).
vec3 mirroredLightDirection(MirroredLight light) {
    float weight = length(light.weightedDirection);
    return weight > 0.0 ? light.weightedDirection / weight : vec3(0.0, 1.0, 0.0);
}

#endif
