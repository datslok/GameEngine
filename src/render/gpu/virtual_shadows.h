#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"
#include "render/gpu/virtual_shadows_shared.h"

#include <array>
#include <cstdint>
#include <optional>

/*
* Virtual shadow maps for the sun (phase 21 stage 4; research and plan: docs/research/VIRTUALSHADOWMAPS.md). Instead of
* four cascades cut along the view, the sun gets a clipmap: square levels centred on the camera, each twice as wide as
* the last, from half a metre to kilometres. Each level is one huge virtual shadow map (4096 or 8192 texels a side) cut
* into pages of 128 x 128 texels, and only the pages some pixel on screen looks up are given memory and drawn (later
* steps); a page keeps its depths until something in it moves.
*
* A point is shadowed by the finest level that holds it, chosen by its distance from the camera alone (as Unreal does
* for the sun), so turning the camera changes no point's level and redraws nothing. With levels of 32 pages a side, a
* texel there is a half to one pixel across on a 1440p screen at 70 degrees.
*
* Each level moves with the camera by whole pages, across the light only, and its pages are addressed by wrapping
* (toroidally): a page of the world keeps its slot in the level's page table while the level slides, so it keeps its
* depths too; only the pages coming in at the far edge are new. Along the light each level has a depth range of its
* own, slack times its half-width either side, re-centred only once the camera has moved most of that along the light:
* one range for all levels would make the finest level's depth steps (a millimetre over kilometres) coarser than its
* texels. Casters up to the caster reach towards the sun beyond a level's range are drawn into it as the cascades'
* boxes do; further ones are the far shadow map's (distant occluders, sun-shadows.md).
*
* This file is the maths: which level, where a point lands in it, which slot of the page table its page takes, and the
* clipmap's state from frame to frame. The shader side is assets/shaders/virtual_shadows.glsl, mirrored here.
*/

inline constexpr int virtualShadowPageTexels = VIRTUAL_SHADOW_PAGE_TEXELS;
inline constexpr int maxVirtualShadowLevels = VIRTUAL_SHADOW_MAX_LEVELS;
inline constexpr int virtualShadowMarginPages = VIRTUAL_SHADOW_MARGIN_PAGES;
inline constexpr int virtualShadowNoLevel = VIRTUAL_SHADOW_NO_LEVEL;

// How many pages a level has a side: 32 (4096 texels, about a texel a pixel at 1440p) or 64 (8192, for 4K screens).
enum class VirtualShadowResolution {
    Pages32,
    Pages64,
};

inline constexpr std::array<const char*, 2> virtualShadowResolutionNames = {"32 pages (4096)", "64 pages (8192)"};

int virtualShadowPagesPerSide(VirtualShadowResolution resolution);

struct VirtualShadowSettings {
    // The finest level's half-width: everything nearer the camera than about this shares its texels.
    float firstHalfWidth = 0.5f;

    // How many levels, each twice as wide as the last: 15 from 0.5 m reach 8 km; past the last, the far shadow map.
    int levelCount = 15;

    VirtualShadowResolution resolution = VirtualShadowResolution::Pages32;

    // Each level's depth range either side of its centre along the light, in its half-widths. The level is re-centred
    // (all its pages dropped) once the camera has moved slack - 1 half-widths along the light: at 16, the finest level
    // every 7.5 m, a 128 m one every 1.9 km. Depth steps grow with the range, but stay far below a texel up to 1000.
    float depthSlack = 16.0f;

    bool operator==(const VirtualShadowSettings&) const = default;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(VirtualShadowSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for settings no clipmap can be built from.
void validateVirtualShadows(const VirtualShadowSettings& settings);

// A level's half-width, page and texel: each level twice the last.
float virtualShadowHalfWidth(float firstHalfWidth, int level);
float virtualShadowPageWidth(float halfWidth, int pagesPerSide);
float virtualShadowTexelSize(float halfWidth, int pagesPerSide);

// How far from the camera a level holds points: its half-width less the margin's pages (VIRTUAL_SHADOW_MARGIN_PAGES).
float virtualShadowUsableHalfWidth(float halfWidth, int pagesPerSide);

/*
* The finest level holding a point this far from the camera, or virtualShadowNoLevel past the last. Found by doubling
* the first level's usable half-width rather than by a logarithm: doubling is exact in floats, so the CPU and the GPU
* (whose log2 is not) choose alike to the last bit, and the marking and sampling passes can never disagree.
*/
int virtualShadowLevelFor(float distance, float firstUsableHalfWidth, int levelCount);

// A page of a level (in pages from its corner), or a slot of its page table.
struct VirtualShadowPage {
    int x = 0;
    int y = 0;

    bool operator==(const VirtualShadowPage&) const = default;
};

// The page a point at these texels of a level lands on.
VirtualShadowPage virtualShadowPageOf(float texelX, float texelY);

// The table slot of a level's page: wrapped by the level's first page's slot, so a page of the world keeps its slot
// while the level slides. Both are 0 to pagesPerSide - 1.
VirtualShadowPage virtualShadowSlot(const VirtualShadowPage& pageInLevel, const VirtualShadowPage& originSlot, int pagesPerSide);

// Where a slot's entry sits in the page table: the levels one after another, rows of a level one after another.
int virtualShadowTableIndex(int level, const VirtualShadowPage& slot, int pagesPerSide);

// One level's state: its first (lowest) page across the light in pages of the world, and the centre of its depth range
// along the light (metres along the light's direction from the world's origin). Kept in doubles: the world can be
// kilometres wide and the finest page 3 cm.
struct VirtualShadowLevel {
    std::int64_t firstPageX = 0;
    std::int64_t firstPageY = 0;
    double depthCentre = 0.0;

    bool operator==(const VirtualShadowLevel&) const = default;
};

// The clipmap from frame to frame: what its pages were drawn for.
struct VirtualShadowClipmap {
    Vec3 lightDirection;
    VirtualShadowSettings settings;
    float casterReach = 0.0f;
    std::array<VirtualShadowLevel, maxVirtualShadowLevels> levels{};
};

// The clipmap after the camera moved, and the levels whose every page is stale (bit per level): all of them for a new
// clipmap, a turned light or changed settings; a level whose depth range was re-centred.
struct VirtualShadowClipmapUpdate {
    VirtualShadowClipmap clipmap;
    std::uint32_t droppedLevels = 0;
};

VirtualShadowClipmapUpdate updateVirtualShadowClipmap(const std::optional<VirtualShadowClipmap>& previous, const Vec3& camera,
                                                      const Vec3& lightDirection, const VirtualShadowSettings& settings,
                                                      float casterReach);

/*
* What the shader needs to place a point in a level this frame, measured from the camera (the main pass's points are):
* a point's light-space position (sunLightRotation times the point less the camera) times inverseTexel plus cameraTexel
* is its texel in the level; its z times depthScale plus depthOffset its depth there (reversed: 1 at the range's sunward
* end, caster reach included, 0 at its far end). originSlot is the level's first page's table slot.
*/
struct VirtualShadowPlacement {
    float cameraTexelX = 0.0f;
    float cameraTexelY = 0.0f;
    float inverseTexel = 0.0f;
    float depthScale = 0.0f;
    float depthOffset = 0.0f;
    VirtualShadowPage originSlot;
};

VirtualShadowPlacement virtualShadowPlacement(const VirtualShadowClipmap& clipmap, int level, const Vec3& camera);

// A point's texel (x, y) and depth (z) in a level, from its light-space position measured from the camera.
Vec3 virtualShadowLevelPoint(const Vec3& lightPoint, const VirtualShadowPlacement& placement);
