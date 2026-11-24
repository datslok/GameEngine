# Virtual shadow maps

Covers: `src/render/gpu/virtual_shadows`, `src/render/vulkan/vulkan_virtual_shadows`, `assets/shaders/virtual_shadows.glsl`, `assets/shaders/virtual_shadow_data.glsl`, `assets/shaders/virtual_shadow_pages.glsl`, `assets/shaders/virtual_shadow_clear.comp`, `assets/shaders/virtual_shadow_mark.comp`, `assets/shaders/virtual_shadow_resolve.comp`, `assets/shaders/virtual_shadow_allocate.comp`

The sun's virtual shadow maps (phase 21 stage 4), replacing the cascades step by step; research, choices and the steps: `docs/research/VIRTUALSHADOWMAPS.md`. Built so far: the clipmap's maths (4a), the pages marked and given room on the GPU (4b). The cascades (`sun-shadows.md`) still draw every shadow.

## The clipmap (`render/gpu/virtual_shadows`, mirrored by `virtual_shadows.glsl`)

- **Levels:** `VirtualShadowSettings::levelCount` (15) squares round the camera, half-widths `firstHalfWidth` (0.5 m) times 2^level (`virtualShadowHalfWidth`): 0.5 m to 8 km.
- **Pages:** `VIRTUAL_SHADOW_PAGE_TEXELS` (128) square; `resolution` 32 pages a side (4096 texels, `Pages32`) or 64 (8192, for 4K screens), `virtualShadowPagesPerSide`. `virtualShadowPageWidth`, `virtualShadowTexelSize`: at 32, the finest level's texel is 0.24 mm, the 8 km one's 4 m.
- **Light frame:** `sunLightRotation` (`cascades.h`, shared with the cascades' boxes): x and y across the light, -z along it.

| Function | What |
|---|---|
| `virtualShadowUsableHalfWidth` | a level's half-width less `VIRTUAL_SHADOW_MARGIN_PAGES` (1) page: half a page for the camera's snapping, half for a soft shadow's filter |
| `virtualShadowLevelFor` | the finest level whose usable half-width holds a distance from the camera; `VIRTUAL_SHADOW_NO_LEVEL` (-1) past the last (the far map's) |
| `virtualShadowPageOf` | a level's texel to its page |
| `virtualShadowSlot` | a page's table slot: plus the level's origin slot, wrapped |
| `virtualShadowTableIndex` | the slot's entry: levels, then rows, one after another |
| `virtualShadowLevelPoint` | a camera-relative light-space point to its texel and reversed depth in a level |

- **By distance, by doubling:** turning the camera changes no point's level. The usable half-width is doubled level by level instead of taking a logarithm: exact in floats, so CPU and GPU (whose `log2` is not exact) choose alike to the bit.
- **Texels follow pixels:** at 32 pages, 1440p and 70 degrees, a texel is 0.5 to 1.1 pixels from 1 m to 4 km (a pixel is d / 1028).

## From frame to frame (CPU only)

`updateVirtualShadowClipmap(previous, camera, light, settings, casterReach)` gives the `VirtualShadowClipmap` (light, settings, reach, a `VirtualShadowLevel` a level) and `droppedLevels` (a bit a level whose every page is stale).

- **Across the light:** a level's first page (`firstPageX`, `firstPageY`, `int64`, pages of the world) is the camera's page rounded, less half the side: the camera within half a page of the centre. Sliding drops nothing: wrapped slots keep a world page in its slot.
- **Along the light:** each level its own depth range, `depthSlack` (16) half-widths either side of `depthCentre`, plus the caster reach sunwards. Kept while the camera is within slack - 1 half-widths of the centre, else re-centred on the camera and dropped. One range for every level would make the finest level's depth steps (1 mm over 16 km) nearly four of its texels; per level they stay under a quarter of a texel.
- **All dropped:** a new clipmap, a light turned at all, new settings or reach.
- **Doubles:** the camera's light-space place, pages and centres are worked out in doubles (`lightPlaceOf`); `virtualShadowPlacement` turns them into small floats measured from the camera: the camera's texel in the level, the inverse texel, depth scale and offset, the origin slot.

## The pages (`VulkanVirtualShadows`, Passes VSMPAGES)

`VirtualShadowSettings` (F5 Virtual shadows, `engine.render.virtualShadows.*`): `enabled` (off until 4c draws), `showPages`, `poolPages` (4096, up to `VIRTUAL_SHADOW_MAX_POOL_PAGES` 16384), and the clipmap's.

- **`prepare`** (before the scene set): the clipmap moved (`updateVirtualShadowClipmap`); each dropped level's generation bumped; this frame's `GpuVirtualShadowFrame` (std430, 1200 B: light rotation, inverse view-projection, screen, options, counts, a `GpuVirtualShadowLevel` a level) written by `packVirtualShadowFrame`. Runs with the prepass and a shadowed sun; else the record holds no levels.
- **Entries** (`uint`): `ALLOCATED`, `REQUESTED`, `DIRTY`, `FAILED` in the top bits, the physical page in the low 16. Each has a key (`VirtualShadowKey`: the world page across the light, low 32 bits, and the level's generation): a key that no longer matches its slot's is stale.
- **On the GPU from frame to frame:** the table (sized for 16 levels of 64²), keys, counters (`VIRTUAL_SHADOW_COUNT_*`), the free list and its top. Other levels, resolution or pool start everything again (`options.z`: all empty, all free).

| Pass | One invocation | Does |
|---|---|---|
| `virtual_shadow_clear.comp` | an entry (or pool page) | asks, dirt, failures cleared; counters to 0 but the free top |
| `virtual_shadow_mark.comp` | a pixel (8 × 8) | the point from the prepass's depth, its level by distance; its page and those within `VIRTUAL_SHADOW_FILTER_TEXELS` (24) asked for (`virtualShadowMarkedPages`, 1-4); skipped if already asked, one write for a wave on one page |
| `virtual_shadow_resolve.comp` | an entry | stale: freed (`virtualShadowEntryStale`); asked with a place: kept; asked without: listed needing (`virtualShadowEntryNeeds`); a place not asked for: listed cached |
| `virtual_shadow_allocate.comp` | one group of 256 | the k-th needing entry takes `virtualShadowAllocationSource`: the k-th free page from the top, else the (k - free)-th cached one's place (emptied), else `FAILED`; new places `DIRTY`; the free top lowered behind a barrier |

- Memory barriers between the passes (`memoryBarrier`, `buffer_barrier.h`, shared with cloth, the mesh pool and ray tracing); the counters copied back per frame in flight.
- **Eviction order** is the GPU's (as Sakmary's), not least recently used: changed if pages flicker in and out.
- **F4:** `VSM PAGES` asked, `KEPT`, `NEW` (`EVICTED` of them), `FAILED`, `STALE`, `FREE`, under the tiles' line.
- **Debug view** (`showPages`, main pass, `virtualShadowPageView` in `scene_surface.glsl`, set 1 bindings 59-60): each pixel its level's colour (`virtualShadowLevelColour`), its page's edges dark (`virtualShadowNearPageEdge`, 2 texels), new pages white, failed red, unasked black.

## Tests

`virtual_shadows_test` (settings and their keys; sizes; the level choice at and a float step past every reach; texels within 0.5-1.1 pixels; points at every level landing half a page inside it, depth inside the range and a caster the reach sunwards too, the worst case straight across the light, cameras 6 km out; walking keeps a world point's slot, texel and depth; which levels drop; slots and table entries each once, negative pages wrapped; depth steps under a quarter texel; marked spans by edges and corners; slots back to pages; entries, sources; the four passes simulated over frames on the CPU with the shared functions: places kept while asked, none stale while walking, a dropped level freed, a full pool evicting then failing, no place held twice; the frame record; the counters and F4's line); GPU: `virtual_shadows_gpu_test` (level choice exact, pages, slots, entries, keys, sources, colours, edges, level points against the mirror).
