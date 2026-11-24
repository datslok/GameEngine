#ifndef VIRTUAL_SHADOW_PAGES_GLSL
#define VIRTUAL_SHADOW_PAGES_GLSL

// The buffers the pages' passes share (render/vulkan/vulkan_virtual_shadows.h), at set 0: the clipmap's record, the page
// table, each entry's key, the counters (VIRTUAL_SHADOW_COUNT_*) and the three lists: the free physical pages (a stack
// its counter tops), the entries needing a page and the entries only cached, this frame's.

#include "virtual_shadow_data.glsl"
#include "virtual_shadows.glsl"

layout(std430, set = 0, binding = 0) readonly buffer VirtualShadowFrame {
    VIRTUAL_SHADOW_FRAME_FIELDS
} frame;

layout(std430, set = 0, binding = 1) coherent buffer VirtualShadowTable {
    uint entries[];
} table;

// x, y the world page across the light, z the level's generation, w the frame it was last asked for.
layout(std430, set = 0, binding = 2) buffer VirtualShadowKeys {
    uvec4 keys[];
} keys;

layout(std430, set = 0, binding = 3) coherent buffer VirtualShadowCounters {
    uint counters[VIRTUAL_SHADOW_COUNTERS];
};

layout(std430, set = 0, binding = 4) buffer VirtualShadowFree {
    uint freePages[];
};

layout(std430, set = 0, binding = 5) buffer VirtualShadowNeeded {
    uint neededEntries[];
};

layout(std430, set = 0, binding = 6) buffer VirtualShadowCached {
    uint cachedEntries[];
};

// How many entries the table has: every level's pages.
int virtualShadowEntryCount() {
    return frame.counts.x * frame.counts.y * frame.counts.y;
}

// The key an entry's slot has this frame: which page of the world its level's slide puts there, and the generation.
uvec3 virtualShadowCurrentKey(int index) {
    int pagesPerSide = frame.counts.y;
    int level = index / (pagesPerSide * pagesPerSide);
    int within = index - level * pagesPerSide * pagesPerSide;
    ivec2 slot = ivec2(within % pagesPerSide, within / pagesPerSide);
    VirtualShadowLevelData data = frame.levels[level];
    return virtualShadowKeyOf(virtualShadowPageOfSlot(slot, data.pages.xy, pagesPerSide), data.pages.zw, data.key.x);
}

#endif
