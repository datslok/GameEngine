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

// The pages a pixel at these texels marks: those its texel's square of reach texels each way touches, clamped to the
// level (xy the first page, zw the last).
ivec4 virtualShadowMarkedPages(vec2 texel, float reach, int pagesPerSide) {
    ivec2 low = clamp(virtualShadowPageOf(texel - reach), ivec2(0), ivec2(pagesPerSide - 1));
    ivec2 high = clamp(virtualShadowPageOf(texel + reach), ivec2(0), ivec2(pagesPerSide - 1));
    return ivec4(low, high);
}

// Which page of its level a table slot holds now (the reverse of virtualShadowSlot).
ivec2 virtualShadowPageOfSlot(ivec2 slot, ivec2 originSlot, int pagesPerSide) {
    return (slot - originSlot + pagesPerSide) % pagesPerSide;
}

// The key of a level's page this frame: the world page across the light (wrapping 32-bit sums) and the generation.
uvec3 virtualShadowKeyOf(ivec2 pageInLevel, ivec2 firstPage, uint generation) {
    return uvec3(uvec2(firstPage) + uvec2(pageInLevel), generation);
}

// An entry with a place in the pool whose key is no longer its slot's: its depths are another page's.
bool virtualShadowEntryStale(uint entry, uvec3 stored, uvec3 current) {
    return (entry & VIRTUAL_SHADOW_ENTRY_ALLOCATED) != 0u && any(notEqual(stored, current));
}

// An entry a pixel asked for that has no place, or only a stale one.
bool virtualShadowEntryNeeds(uint entry, bool stale) {
    return (entry & VIRTUAL_SHADOW_ENTRY_REQUESTED) != 0u && ((entry & VIRTUAL_SHADOW_ENTRY_ALLOCATED) == 0u || stale);
}

uint virtualShadowPhysicalPage(uint entry) {
    return entry & VIRTUAL_SHADOW_ENTRY_PHYSICAL;
}

// Where the k-th entry needing a place gets one: x VIRTUAL_SHADOW_FROM_FREE (y the free list's index, from its top),
// VIRTUAL_SHADOW_FROM_CACHED (y the cached list's index) or VIRTUAL_SHADOW_FROM_NOWHERE (the pool is full).
uvec2 virtualShadowAllocationSource(uint k, uint freeTop, uint cached) {
    if (k < freeTop) {
        return uvec2(VIRTUAL_SHADOW_FROM_FREE, freeTop - 1u - k);
    }

    if (k - freeTop < cached) {
        return uvec2(VIRTUAL_SHADOW_FROM_CACHED, k - freeTop);
    }

    return uvec2(VIRTUAL_SHADOW_FROM_NOWHERE, 0u);
}

uint virtualShadowEntryWith(uint physicalPage, uint flags) {
    return (flags & VIRTUAL_SHADOW_ENTRY_FLAGS) | (physicalPage & VIRTUAL_SHADOW_ENTRY_PHYSICAL);
}

// The pages' debug view: a colour a level, six in turn (none red: red is a page the pool had no room for).
vec3 virtualShadowLevelColour(int level) {
    const vec3 colours[6] = vec3[6](vec3(0.2, 0.4, 1.0), vec3(0.2, 0.9, 0.3), vec3(1.0, 0.9, 0.2), vec3(0.2, 0.9, 0.9),
                                    vec3(1.0, 0.55, 0.1), vec3(0.65, 0.3, 1.0));
    return colours[level % 6];
}

// Whether a level's texel lies within width texels of its page's edge (the debug view's outlines).
bool virtualShadowNearPageEdge(vec2 texel, float width) {
    vec2 within = texel - floor(texel / float(VIRTUAL_SHADOW_PAGE_TEXELS)) * float(VIRTUAL_SHADOW_PAGE_TEXELS);
    return any(lessThan(within, vec2(width))) || any(greaterThan(within, vec2(float(VIRTUAL_SHADOW_PAGE_TEXELS) - width)));
}

#endif
