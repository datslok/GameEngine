#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/virtual_shadows_shared.h"

#include <array>
#include <cstddef>
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
    // The pages marked and given room each frame (ROADMAP phase 21 stage 4b); nothing is drawn into them until 4c, so
    // off by default and on only to look at the pages' debug view and F4's VSM line.
    bool enabled = false;

    // A debug view: each pixel tinted by its level, its page's edges outlined, pages given room this frame white, pages
    // the pool had no room for red.
    bool showPages = false;

    // The pool's pages, shared by every level (Unreal's default 4096: 256 MB of 32-bit depth once drawn into, 4c).
    int poolPages = 4096;

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

/*
* The sun's casters (stage 4c-i), culled once for every level the screen may mark, as one more shadow view of the
* camera's culling. The levels: up to the finest holding the farthest surface seen (a point that far along the view at
* the screen's corner, where it is farthest from the camera), every level when none holds it.
*/
int virtualShadowCasterLevels(const VirtualShadowSettings& settings, float farthestAlongView, float verticalFov, float aspectRatio);

/*
* The box those levels' casters lie in, as a shadow tile's world-space clip matrix (reversed depth; x, y across the
* light): every level's pages across, and along the light every level's whole depth range, the caster reach sunwards
* included, so a page drawn holds every caster its depths can (and, once pages are cached, still holds them while the
* camera moves within the range).
*/
Mat4 virtualShadowCasterBox(const VirtualShadowClipmap& clipmap, int levels);

/*
* The pages (stage 4b). Each frame the marking pass flags the page under every pixel on screen, and the pages round it
* a soft shadow's filter may read (VIRTUAL_SHADOW_FILTER_TEXELS); the resolving pass then decides each table entry's
* fate, and the allocating pass gives the pages that need one a place in the pool: a free one, else one only cached.
* An entry remembers which page of the world its place holds (its key: the world page across the light and the level's
* generation, bumped whenever the level drops its pages), so a page that slid out of its level or was dropped is known
* by its key no longer matching and goes back to the free list.
*/
inline constexpr std::uint32_t virtualShadowEntryAllocated = VIRTUAL_SHADOW_ENTRY_ALLOCATED;
inline constexpr std::uint32_t virtualShadowEntryRequested = VIRTUAL_SHADOW_ENTRY_REQUESTED;
inline constexpr std::uint32_t virtualShadowEntryDirty = VIRTUAL_SHADOW_ENTRY_DIRTY;
inline constexpr std::uint32_t virtualShadowEntryFailed = VIRTUAL_SHADOW_ENTRY_FAILED;
inline constexpr int virtualShadowFilterTexels = VIRTUAL_SHADOW_FILTER_TEXELS;
inline constexpr int maxVirtualShadowPoolPages = VIRTUAL_SHADOW_MAX_POOL_PAGES;

// The pages a pixel at these texels of a level marks: those its texel's square of reach texels each way touches,
// clamped to the level (x, y the first page, z, w the last).
struct VirtualShadowPageSpan {
    VirtualShadowPage first;
    VirtualShadowPage last;

    bool operator==(const VirtualShadowPageSpan&) const = default;
};

VirtualShadowPageSpan virtualShadowMarkedPages(float texelX, float texelY, float reach, int pagesPerSide);

// The reverse of virtualShadowSlot: which page of its level a table slot holds now.
VirtualShadowPage virtualShadowPageOfSlot(const VirtualShadowPage& slot, const VirtualShadowPage& originSlot, int pagesPerSide);

// Which page of the world (across the light, low 32 bits) and which drawing of its level a table entry stands for.
struct VirtualShadowKey {
    std::uint32_t pageX = 0;
    std::uint32_t pageY = 0;
    std::uint32_t generation = 0;

    bool operator==(const VirtualShadowKey&) const = default;
};

// The key a level's page has this frame: the level's first page plus the page, and the level's generation.
VirtualShadowKey virtualShadowKeyOf(const VirtualShadowPage& pageInLevel, std::int32_t firstPageX, std::int32_t firstPageY, std::uint32_t generation);

// An entry with a place in the pool whose key is no longer its slot's: its depths are another page's, so it is freed.
bool virtualShadowEntryStale(std::uint32_t entry, const VirtualShadowKey& stored, const VirtualShadowKey& current);

// An entry a pixel asked for that has no place (or only a stale one): it needs one from the pool.
bool virtualShadowEntryNeeds(std::uint32_t entry, bool stale);

std::uint32_t virtualShadowPhysicalPage(std::uint32_t entry);

/*
* Where the k-th entry needing a place gets one, with freeTop pages on the free list and cached entries listed: the free
* list's k-th page from the top (VIRTUAL_SHADOW_FROM_FREE, the list's index), else the (k - freeTop)-th cached entry's
* place (VIRTUAL_SHADOW_FROM_CACHED, the cached list's index), else none: the pool is full (VIRTUAL_SHADOW_FROM_NOWHERE).
*/
struct VirtualShadowSource {
    int from = VIRTUAL_SHADOW_FROM_NOWHERE;
    std::uint32_t index = 0;

    bool operator==(const VirtualShadowSource&) const = default;
};

VirtualShadowSource virtualShadowAllocationSource(std::uint32_t k, std::uint32_t freeTop, std::uint32_t cached);
std::uint32_t virtualShadowEntryWith(std::uint32_t physicalPage, std::uint32_t flags);

// The pages' debug view (the main pass, while VirtualShadowSettings::showPages): a colour a level, six in turn (none
// red: red marks a page the pool had no room for), and whether a texel lies within width texels of its page's edge.
Vec3 virtualShadowLevelColour(int level);
bool virtualShadowNearPageEdge(float texelX, float texelY, float width);

// What the resolving and allocating passes counted this frame (VIRTUAL_SHADOW_COUNT_*), for F4's VSM line.
struct VirtualShadowCounts {
    std::uint32_t requested = 0;
    std::uint32_t kept = 0;
    std::uint32_t allocated = 0;
    std::uint32_t evicted = 0;
    std::uint32_t failed = 0;
    std::uint32_t stale = 0;
    std::uint32_t free = 0;
};

VirtualShadowCounts virtualShadowCountsFrom(const std::array<std::uint32_t, VIRTUAL_SHADOW_COUNTERS>& counters);

// The sun's casters (stage 4c-i), for F4's VSM CASTERS line: the draws its view took, the clusters tested and kept of
// them, and the kept ones' triangles.
struct VirtualShadowCasters {
    std::size_t draws = 0;
    std::size_t tested = 0;
    std::size_t kept = 0;
    std::size_t triangles = 0;
};

// One level for the shader (std430): virtualShadowPlacement's numbers, the level's first page (low 32 bits) and
// generation for the keys.
struct GpuVirtualShadowLevel {
    float placement[4];   // x, y the camera's texel, z the inverse texel size, w the depth scale
    float depth[4];       // x the depth offset
    std::int32_t pages[4]; // x, y the first page's table slot, z, w the first page across the light (low 32 bits)
    std::uint32_t key[4]; // x the level's generation
};

static_assert(sizeof(GpuVirtualShadowLevel) == 64, "GpuVirtualShadowLevel must match virtual_shadow_data.glsl's");

// The clipmap for the passes and the main pass (std430, set 1 binding SCENE_VIRTUAL_SHADOW_FRAME_BINDING).
struct GpuVirtualShadowFrame {
    float lightRotation[16];          // sunLightRotation, column-major: camera-relative points to the light's axes
    float inverseViewProjection[16];  // the camera's, camera-relative and jittered: pixels back to points
    float screen[4];                  // the render size in pixels, and its inverse
    float options[4];                 // x the first level's usable half-width, y 1 shows the pages' debug view, z 1 starts the pages again
    std::int32_t counts[4];           // x the levels, y the pages a level's side, z the pool's pages, w the frame
    GpuVirtualShadowLevel levels[maxVirtualShadowLevels];
};

static_assert(sizeof(GpuVirtualShadowFrame) == 1200, "GpuVirtualShadowFrame must match virtual_shadow_data.glsl's");

// The frame's record: the clipmap placed for this camera, each level's generation, the frame's counter.
GpuVirtualShadowFrame packVirtualShadowFrame(const VirtualShadowClipmap& clipmap, const Vec3& camera,
                                             const std::array<std::uint32_t, maxVirtualShadowLevels>& generations,
                                             const Mat4& inverseViewProjection, float width, float height, bool showPages,
                                             std::uint32_t frame);
