// The light probes' atlases in set 1 (render/gpu/ddgi_sampling.h), and sampling them at a surface: the corner loop that
// reads the atlases around ddgi_sampling.glsl's weights. Included by triangle.frag and ddgi_trace.comp, after
// frame_data.glsl (the volume's numbers are in the frame's uniform block). Mirrored by sampleProbes in C++.
#ifndef SCENE_PROBES_GLSL
#define SCENE_PROBES_GLSL

#include "ddgi_sampling.glsl"

// Bound to an empty stand-in while the probes are not sampled (nothing reads them then).
layout(set = 1, binding = 12) uniform sampler2D probeIrradianceAtlas;
layout(set = 1, binding = 13) uniform sampler2D probeDistanceAtlas;

// What the probes give a surface: irradiance for its diffuse part, the same read in the mirror direction for its specular part.
struct ProbeLight {
    vec3 diffuse;
    vec3 specular;
};

/*
* The probes' light at a point with this normal, seen from toViewer (for a probe ray's hit, back along the ray): the 8
* corner probes of the cell round the biased point, each read in the right direction and weighted. lookupNormal is the
* direction the irradiance is read in, reflected the mirror direction (read only withSpecular, a constant at each call,
* so a matte caller pays nothing for it). origin is the first probe in the point's own space (measured from the camera
* on screen, the world for probe rays).
*/
ProbeLight sampleProbes(vec3 point, vec3 normal, vec3 lookupNormal, vec3 reflected, vec3 toViewer, vec3 origin, bool withSpecular) {
    vec3 spacing = probeSpacing.xyz;
    ivec3 counts = probeCounts.xyz;
    vec3 biased = point + probeSelfShadowBias(normal, toViewer, spacing, probeOrigin.w);
    ProbeCell cell = probeCell(origin, spacing, counts, biased);
    ProbeBlend diffuse = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend specular = ProbeBlend(vec3(0.0), 0.0);

    for (int corner = 0; corner < 8; ++corner) {
        ivec3 grid = probeCellCorner(cell, counts, corner);
        int index = grid.x + counts.x * (grid.y + counts.y * grid.z);
        vec3 probe = origin + spacing * vec3(grid);
        vec3 fromProbe = biased - probe;
        float distance = length(fromProbe);
        vec3 direction = distance > 0.0 ? fromProbe / distance : normal;

        vec2 moments = textureLod(probeDistanceAtlas, probeAtlasUv(counts, index, direction, DDGI_DISTANCE_TEXELS), 0.0).rg;
        float weight = probeCornerWeight(normal, probe - point, distance, moments, probeTrilinearWeight(cell.alpha, corner));
        addProbeSample(diffuse, textureLod(probeIrradianceAtlas, probeAtlasUv(counts, index, lookupNormal, DDGI_IRRADIANCE_TEXELS), 0.0).rgb, weight);

        if (withSpecular) {
            addProbeSample(specular, textureLod(probeIrradianceAtlas, probeAtlasUv(counts, index, reflected, DDGI_IRRADIANCE_TEXELS), 0.0).rgb, weight);
        }
    }

    return ProbeLight(finishProbeBlend(diffuse), withSpecular ? finishProbeBlend(specular) : vec3(0.0));
}

/*
* The light from all around at a point: the probes' where the volume reaches, blended into the flat ambient across the
* volume's edge, and the flat ambient alone beyond it.
*/
ProbeLight probeAmbient(vec3 point, vec3 normal, vec3 lookupNormal, vec3 reflected, vec3 toViewer, vec3 origin, bool withSpecular) {
    float inVolume = probeVolumeWeight(origin, probeSpacing.xyz, probeCounts.xyz, point);

    if (inVolume <= 0.0) {
        return ProbeLight(ambient.rgb, ambient.rgb);
    }

    ProbeLight probes = sampleProbes(point, normal, lookupNormal, reflected, toViewer, origin, withSpecular);
    return ProbeLight(mix(ambient.rgb, probes.diffuse, inVolume), mix(ambient.rgb, probes.specular, inVolume));
}

#endif
