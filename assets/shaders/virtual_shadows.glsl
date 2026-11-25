#ifndef VIRTUAL_SHADOWS_GLSL
#define VIRTUAL_SHADOWS_GLSL

// The sun's virtual shadow maps: which level holds a point, where it lands there, which slot of the page table its page
// takes (render/gpu/virtual_shadows.h mirrors it; the design is there and in docs/research/VIRTUALSHADOWMAPS.md).

#include "render/gpu/virtual_shadows_shared.h"
#include "cascades.glsl"

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

// Drawing into the pages (stage 4c-ii).

// An entry whose page is drawn this frame: a place in the pool, and asked for.
bool virtualShadowEntryDrawn(uint entry) {
    uint drawn = VIRTUAL_SHADOW_ENTRY_ALLOCATED | VIRTUAL_SHADOW_ENTRY_REQUESTED;
    return (entry & drawn) == drawn;
}

// The pages a caster's sphere touches in a level (its centre's texel and depth there, its radius in texels and depth):
// false when it misses the level across the light or lies wholly past the depth range's far end; span xy the first
// page, zw the last.
bool virtualShadowSpherePages(vec3 levelPoint, float radiusTexels, float radiusDepth, int pagesPerSide, out ivec4 span) {
    ivec2 low = virtualShadowPageOf(levelPoint.xy - radiusTexels);
    ivec2 high = virtualShadowPageOf(levelPoint.xy + radiusTexels);
    span = ivec4(clamp(low, ivec2(0), ivec2(pagesPerSide - 1)), clamp(high, ivec2(0), ivec2(pagesPerSide - 1)));
    return !(levelPoint.z + radiusDepth < 0.0) && all(greaterThanEqual(high, ivec2(0))) && all(lessThan(low, ivec2(pagesPerSide)));
}

// A page pair's two words: the caster's place, then its level, page and physical page (VIRTUAL_SHADOW_PAIR_*).
uvec2 virtualShadowPackPair(uint place, int level, ivec2 page, uint physical) {
    uint where = (uint(level) & VIRTUAL_SHADOW_PAIR_LEVEL_MASK) << VIRTUAL_SHADOW_PAIR_LEVEL_SHIFT |
                 (uint(page.y) & VIRTUAL_SHADOW_PAIR_PAGE_MASK) << VIRTUAL_SHADOW_PAIR_PAGE_Y_SHIFT |
                 (uint(page.x) & VIRTUAL_SHADOW_PAIR_PAGE_MASK) << VIRTUAL_SHADOW_PAIR_PAGE_X_SHIFT | (physical & VIRTUAL_SHADOW_ENTRY_PHYSICAL);
    return uvec2(place, where);
}

int virtualShadowPairLevel(uvec2 pair) {
    return int((pair.y >> VIRTUAL_SHADOW_PAIR_LEVEL_SHIFT) & VIRTUAL_SHADOW_PAIR_LEVEL_MASK);
}

ivec2 virtualShadowPairPage(uvec2 pair) {
    return ivec2((pair.y >> VIRTUAL_SHADOW_PAIR_PAGE_X_SHIFT) & VIRTUAL_SHADOW_PAIR_PAGE_MASK, (pair.y >> VIRTUAL_SHADOW_PAIR_PAGE_Y_SHIFT) & VIRTUAL_SHADOW_PAIR_PAGE_MASK);
}

uint virtualShadowPairPhysical(uvec2 pair) {
    return pair.y & VIRTUAL_SHADOW_ENTRY_PHYSICAL;
}

// A level's texel on one of its pages to its texel in the pool, at the page's physical place (poolColumns a row).
vec2 virtualShadowPoolTexel(vec2 texel, ivec2 page, uint physical, int poolColumns) {
    uint columns = uint(poolColumns);
    vec2 place = vec2(float(physical % columns), float(physical / columns)) - vec2(page);
    return texel + place * float(VIRTUAL_SHADOW_PAGE_TEXELS);
}

// The four clip distances keeping a page's triangles on it: in from its left, right, top and bottom edges (texels).
vec4 virtualShadowPageClip(vec2 texel, ivec2 page) {
    vec2 low = vec2(page) * float(VIRTUAL_SHADOW_PAGE_TEXELS);
    vec2 high = low + float(VIRTUAL_SHADOW_PAGE_TEXELS);
    return vec4(texel.x - low.x, high.x - texel.x, texel.y - low.y, high.y - texel.y);
}

// A pool texel and depth in clip space, for the flipped viewport every pass sets (its top at y = 0).
vec4 virtualShadowPoolClip(vec2 poolTexel, float depth, ivec2 poolSize) {
    return vec4(2.0 * poolTexel.x / float(poolSize.x) - 1.0, 1.0 - 2.0 * poolTexel.y / float(poolSize.y), depth, 1.0);
}

// The main pass's lookup (stage 4c-iii).

// The cascades' bias (sunShadowBias) with the level's texel times biasTexels as its base, as a step in the level's depth:
// moving a point along the light changes only its depth.
float virtualShadowBiasDepth(float biasTexels, float inverseTexel, float depthScale, float nDotL) {
    return sunShadowBias(biasTexels / inverseTexel, nDotL) * depthScale;
}

// 0 where the pool holds a depth more than the bias sunward of the point (reversed: larger), else 1.
float virtualShadowLit(float stored, float depth, float biasDepth) {
    return stored > depth + biasDepth ? 0.0 : 1.0;
}

#endif
