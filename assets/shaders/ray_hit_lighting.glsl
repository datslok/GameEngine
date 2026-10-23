// What a ray traced through the scene finds where it hits a front face, shared by the light probes' rays
// (ddgi_trace.comp) and the reflections' rays (reflections_rays.comp): the surface (its normal, matte colour and glow)
// and the light reaching it. Included after ray_scene.glsl, materials.glsl, instances.glsl, frame_data.glsl,
// scene_lights.glsl and scene_probes.glsl, in compute shaders only.

#ifndef RAY_HIT_LIGHTING_GLSL
#define RAY_HIT_LIGHTING_GLSL

// matteColour, for the hit's matte colour.
#include "brdf.glsl"

// The point lights, then the spotlights, whose light may reach the probes' rays' hits: x numbers into the light buffers,
// y 1 for a fast light (render/gpu/ddgi_sampling.h, GpuProbeHitLight).
layout(std430, set = 1, binding = 14) readonly buffer ProbeHitLights {
    ivec2 probeHitLights[];
};

// Listed light number of the hit lights: its position and range, measured from the camera (as the light buffers are).
vec4 hitLightPositionRange(int listed) {
    int light = probeHitLights[listed].x;
    return listed < probeLights.x ? points[light].positionRange : spots[light].positionRange;
}

// Listed light number of the hit lights' offset from a surface, both measured from the camera.
vec3 hitLightOffset(int listed, vec3 relativeHit) {
    return hitLightPositionRange(listed).xyz - relativeHit;
}

// The irradiance listed light number gives the surface, before its shadow ray.
vec3 hitLightIrradiance(int listed, vec3 relativeHit, vec3 normal) {
    int light = probeHitLights[listed].x;
    vec3 offset = hitLightOffset(listed, relativeHit);

    if (listed < probeLights.x) {
        PointLightData point = points[light];
        return pointLightAtHit(offset, point.positionRange.w, point.radiance.rgb, point.radiance.w, normal);
    }

    SpotLightData spot = spots[light];
    return spotLightAtHit(offset, spot.positionRange.w, spot.radianceCosInner.rgb, spot.sourceRadius, spot.directionCosOuter.xyz,
                          spot.directionCosOuter.w, spot.radianceCosInner.w, normal);
}

struct RayHitSurface {
    vec3 normal; // facing the ray
    vec3 albedo;
    vec3 emissive;
};

// The surface a ray along direction met at a triangle of an instance, from the mesh's own vertices.
RayHitSurface rayHitSurface(int instanceIndex, int primitive, vec2 barycentrics, vec3 direction) {
    InstanceData instance = instances[instanceIndex];
    MeshSurface surface = meshSurfaceAt(instance.material.y, primitive, barycentrics);
    vec3 normal = normalize(mat3(instance.normalMatrix) * surface.normal);

    // The face was met from its front, so its shading normal should face the ray too; smoothing can tip it past.
    if (dot(normal, direction) > 0.0) {
        normal = -normal;
    }

    // Matte only: a metal scatters nothing from under its surface, so it is dark here (its reflections are not traced).
    MaterialData material = materials[instance.material.x];
    vec3 baseColour = textureLod(textures[nonuniformEXT(material.textureSlots.x)], surface.uv, 0.0).rgb * material.baseColour.rgb;
    float metallic = clamp(material.surfaceParameters.x * textureLod(textures[nonuniformEXT(material.textureSlots.y)], surface.uv, 0.0).b, 0.0, 1.0);
    return RayHitSurface(normal, matteColour(baseColour, metallic), material.emissive.rgb);
}

// The irradiance reaching a hit, kept apart by whether fast lights (flashlights) gave it.
struct RayHitLight {
    vec3 still;
    vec3 fast;
};

/*
* The light reaching a hit (in the world) facing normal, met by a ray along direction (matte only: the probes' cosine
* averaging blurs gloss away [2019]): every directional light that reaches it and the brightest few point and spot
* lights near the probes (render/gpu/ddgi_sampling.h, probeHitLights), each found by a shadow ray (the shadow atlas only
* holds what the camera sees), and the light from all around as last frame's probes hold it (the flat ambient beyond
* them), times bounce (the probe rays' bounce energy; 1 for reflections, which see the whole of it).
*/
RayHitLight lightAtRayHit(vec3 hit, vec3 normal, vec3 direction, float bounce) {
    vec3 shadowFrom = hit + normal * DDGI_SHADOW_RAY_OFFSET;
    vec3 irradiance = vec3(0.0);

    for (int i = 0; i < counts.x; ++i) {
        vec3 toLight = directional[i].toLight.xyz;
        float facing = dot(normal, toLight);

        if (facing > 0.0 && reachesLight(shadowFrom, toLight, DDGI_RAY_REACH)) {
            irradiance += directional[i].radiance.rgb * facing;
        }
    }

    // The brightest point and spot lights here, then a shadow ray to each (to its centre: its glowing stand-in is not solid).
    vec3 relativeHit = hit - cameraOrigin.xyz;
    BrightestLights kept;
    kept.count = 0;

    for (int listed = 0; listed < probeLights.x + probeLights.y; ++listed) {
        keepBrightestLight(kept, probeLights.z, listed, hitLightStrength(hitLightIrradiance(listed, relativeHit, normal)));
    }

    vec3 fastIrradiance = vec3(0.0);

    for (int slot = 0; slot < kept.count; ++slot) {
        int listed = kept.lights[slot];
        vec3 offset = hitLightOffset(listed, relativeHit + normal * DDGI_SHADOW_RAY_OFFSET);
        float lightDistance = length(offset);

        if (lightDistance > 0.0 && reachesLight(shadowFrom, offset / lightDistance, lightDistance)) {
            if (probeHitLights[listed].y != 0) {
                fastIrradiance += hitLightIrradiance(listed, relativeHit, normal);
            } else {
                irradiance += hitLightIrradiance(listed, relativeHit, normal);
            }
        }
    }

    // Last frame's probes at the hit, every cascade; each atlas's light passed on into its own. Without probes this
    // frame, the flat ambient.
    if (probeAtlas.w > 0) {
        ProbeLights bounced = probeAmbient(relativeHit, normal, normal, normal, -direction, 0, false);
        irradiance += bounced.still.diffuse * bounce;
        fastIrradiance += bounced.fast.diffuse * bounce;
    } else {
        irradiance += flatAmbient() * bounce;
    }

    return RayHitLight(irradiance, fastIrradiance);
}

#ifdef RAY_HIT_MIRRORED_LIGHTS
// Light reflected onto the hit by mirrors (mirrored_light_search.glsl), one footprint sample a hit (the probes' many rays
// and the reflections' frames average them). Kept out of lightAtRayHit and compiled only where it is asked for: the
// search's registers slowed every probe ray by half when it sat in the trace, run or not (ddgi_mirror.comp has it instead).
#include "mirrored_light_search.glsl"

// The irradiance mirrors reflect onto a hit (in the world) facing normal; noisePixel turns its footprint's samples.
vec3 mirroredLightAtRayHit(vec3 hit, vec3 normal, vec2 noisePixel) {
    if (mirrorPairCounts.x == 0) {
        return vec3(0.0);
    }

    vec3 relativeHit = hit - cameraOrigin.xyz;
    MirroredLight mirrored = mirroredLightAt(relativeHit, relativeHit + normal * DDGI_SHADOW_RAY_OFFSET, noisePixel, MIRROR_HIT_FOOTPRINT_SAMPLES);
    return mirrored.total * max(dot(normal, mirroredLightDirection(mirrored)), 0.0);
}
#endif

#endif
