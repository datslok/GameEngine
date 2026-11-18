// GPU-driven drawing's culling, included with #include "culling.glsl": mirrors render/gpu/cull_records.h
// (sphereInFrustum, cameraTakes) and declares the records it reads. Pure functions.
#ifndef CULLING_GLSL
#define CULLING_GLSL

#include "render/gpu/cull_records_shared.h"

// One draw for a culling thread. Must match GpuCullRecord.
struct CullRecord {
    vec4 bounds;           // its bounding sphere in the world: xyz centre, w radius (below 0 unknown)
    vec4 levelSphere;      // the sphere its level is chosen by
    uint flags;            // CULL_* bits
    uint mesh;             // its mesh's number, naming its level table
    float hiddenBelowError; // an impostor's: drawn only where this error fits
    uint unused;
};

// A view's six planes: inward normal xyz, distance w. Must match GpuFrustum.
struct CullFrustum {
    vec4 planes[6];
};

// Whether a sphere may be inside all six planes; unknown bounds always may (Frustum::intersectsSphere).
bool sphereInFrustum(CullFrustum frustum, vec4 sphere) {
    if (sphere.w < 0.0) {
        return true;
    }

    for (int plane = 0; plane < 6; ++plane) {
        vec4 p = frustum.planes[plane];
        float signedDistance = p.x * sphere.x + p.y * sphere.y + p.z * sphere.z + p.w;

        if (signedDistance < -sphere.w) {
            return false;
        }
    }

    return true;
}

// The camera's opaque views take a draw neither shadow-only, hidden nor transparent that may be in view.
bool cameraTakes(CullRecord record, CullFrustum frustum) {
    return (record.flags & (CULL_SHADOW_ONLY | CULL_HIDDEN | CULL_TRANSPARENT)) == 0u && sphereInFrustum(frustum, record.bounds);
}

#endif
