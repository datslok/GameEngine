#ifndef VIRTUAL_SHADOWS_GLSL
#define VIRTUAL_SHADOWS_GLSL

// The sun's virtual shadow maps: which level holds a point, where it lands there, which slot of the page table its page
// takes (render/gpu/virtual_shadows.h mirrors it; the design is there and in docs/research/VIRTUALSHADOWMAPS.md).

#include "render/gpu/virtual_shadows_shared.h"

// The finest level holding a point this far from the camera, or VIRTUAL_SHADOW_NO_LEVEL past the last. By doubling,
// which is exact, not by log2, which is not: the CPU and every pass choose alike to the last bit.
int virtualShadowLevelFor(float distance, float firstUsableHalfWidth, int levelCount) {
    float reach = firstUsableHalfWidth;

    for (int level = 0; level < levelCount; ++level) {
        if (distance <= reach) {
            return level;
        }

        reach *= 2.0;
    }

    return VIRTUAL_SHADOW_NO_LEVEL;
}

// The page a point at these texels of a level lands on.
ivec2 virtualShadowPageOf(vec2 texel) {
    return ivec2(floor(texel / float(VIRTUAL_SHADOW_PAGE_TEXELS)));
}

// The table slot of a level's page, wrapped by the level's first page's slot (both 0 to pagesPerSide - 1).
ivec2 virtualShadowSlot(ivec2 pageInLevel, ivec2 originSlot, int pagesPerSide) {
    return (pageInLevel + originSlot) % pagesPerSide;
}

// Where a slot's entry sits in the page table: the levels one after another, rows of a level one after another.
int virtualShadowTableIndex(int level, ivec2 slot, int pagesPerSide) {
    return (level * pagesPerSide + slot.y) * pagesPerSide + slot.x;
}

// A point's texel (xy) and reversed depth (z) in a level, from its light-space position measured from the camera.
// placement: x, y the camera's texel, z the inverse texel size; depth: x scale, y offset (VirtualShadowPlacement).
vec3 virtualShadowLevelPoint(vec3 lightPoint, vec3 placement, vec2 depth) {
    return vec3(lightPoint.xy * placement.z + placement.xy, lightPoint.z * depth.x + depth.y);
}

#endif
