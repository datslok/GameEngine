# Virtual shadow maps

Covers: `src/render/gpu/virtual_shadows`, `src/render/vulkan/vulkan_virtual_shadows`, `assets/shaders/virtual_shadows.glsl`, `assets/shaders/virtual_shadow_data.glsl`, `assets/shaders/virtual_shadow_pages.glsl`, `assets/shaders/virtual_shadow_clear.comp`, `assets/shaders/virtual_shadow_mark.comp`, `assets/shaders/virtual_shadow_resolve.comp`, `assets/shaders/virtual_shadow_allocate.comp`, `assets/shaders/virtual_shadow_expand.comp`, `assets/shaders/virtual_shadow_tasks.comp`, `assets/shaders/virtual_shadow_draw.mesh`, `assets/shaders/virtual_shadow_cut_out.frag`, `assets/shaders/virtual_shadow_atomic.frag`

The sun's virtual shadow maps (phase 21 stage 4), replacing the cascades step by step; research, choices and the steps: `docs/research/VIRTUALSHADOWMAPS.md`. Built so far: the clipmap's maths (4a), the pages marked and given room on the GPU (4b), the casters culled (4c-i), drawn into the pages (4c-ii), by the depth test or by atomics (4c-iv), and read by the main pass behind a switch (4c-iii). The cascades (`sun-shadows.md`) still draw every frame and shade every point without a drawn page.

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

`VirtualShadowSettings` (F5 Virtual shadows, `engine.render.virtualShadows.*`): `enabled` (off until 4c draws), `showPages`, `mainPass` (off, 4c-iii), `drawing` (atomics by default, or the depth test, 4c-iv), `poolPages` (4096, up to `VIRTUAL_SHADOW_MAX_POOL_PAGES` 16384), and the clipmap's.

- **`prepare`** (before the scene set): the clipmap moved (`updateVirtualShadowClipmap`); each dropped level's generation bumped; this frame's `GpuVirtualShadowFrame` (std430, 1328 B: light rotation, inverse view-projection, screen, options, counts, pool, shading, the camera's culling planes, a `GpuVirtualShadowLevel` a level) written by `packVirtualShadowFrame`. Runs with the prepass and a shadowed sun; else the record holds no levels.
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

## The casters (4c-i, Passes VSMCULL)

With the pages on and mesh shaders, the sun gets one more shadow view in the camera's culling (`gpu-driven.md`), its clusters culled after the camera's by passes of their own.

- **Levels:** `virtualShadowCasterLevels`: up to the finest holding the farthest surface seen (the cascades' fitted distance, else the far plane, at the screen's corner: times sqrt(1 + t^2)); every level past the last.
- **Box:** `virtualShadowCasterBox`: those levels' pages across and their whole depth ranges along, caster reach sunwards, as a world clip matrix (a level's every caster for a page drawn now or cached later).
- **View:** `packVirtualShadowCull` (`shadow_culling`): `CAMERA_CULL_SHADOW`, any caster size, no reach or cascade; its levels the camera's, so its old levels and its clusters' cut are what the camera draws (a caster the same in every level, and as seen). Its list a draw list region; a placed view (`setPlacedViews`), and a swaying one (an off-screen tree sways in its shadow).
- **Clusters:** a second `VulkanMeshClusterCulling` (`virtualShadowClusters`; pyramids one texel, no occlusion, no twigs), inputs shared with the camera's (`meshClusterInputs`); the placed commands from the view's first (`ClusterInputs::commandFirst`, `placedSetup.x`). A shadow view never culls by facing.
- **F4:** `VSM CASTERS` draws taken, clusters kept/tested, triangles, after the pages' line.
- Leaves (twigs, far leaves) not before 4f.

## Drawing into the pages (4c-ii, Passes VSMDRAW)

After the pages are given room, with the casters culled; every drawn page (`virtualShadowEntryDrawn`: a place and asked for) is drawn each frame until 4e caches them.

| Step | What |
|---|---|
| Expand (`virtual_shadow_expand.comp`) | a workgroup a survivor of one style's run (dispatched by the casters' list's own task command for that style; `listPassBindings` gives it the cluster passes' buffers), a thread a level: the cluster's sphere (posed; a draw kept whole, `meshClusterSeenWhole`, its draw's bounds; unknown bounds the whole level) laid into the level (`virtualShadowSpherePages`: none off the level or wholly past its far end), each drawn page a pair; mirrored by `appendVirtualShadowPagePairs` |
| Tasks (`virtual_shadow_tasks.comp`) | one thread: each list's mesh shader draw (`meshClusterTaskGroups` of its pairs within the room) |
| Draw (`virtual_shadow_draw.mesh`) | a workgroup a pair: its cluster placed by `drawPooledCluster` (the camera's mesh shaders' body: sway, bones), each vertex then into the light's axes, the level (`virtualShadowLevelPoint`), its page's place in the pool (`virtualShadowPoolTexel`) and clip space (`virtualShadowPoolClip`, for the flipped viewport); four clip distances at the page's edges (`virtualShadowPageClip`) |

- **Pairs** (`VirtualShadowPagePair`, two words: the caster's place in its list; level 4 bits, page 6 + 6, physical page 16): two lists, plain (no fragment shader) and cut-outs (`virtual_shadow_cut_out.frag`, the prepass's test shared through `cut_out_prepass.glsl`: holes, impostors' cards, fading levels). The buffer, a frame in flight's own: a header (`VIRTUAL_SHADOW_HEADER_*`: each list's pairs found, triangles, each list's draw), then each list's room. Appended by atomics: their order changes, the depths do not (GREATER keeps the sunward depth whichever comes first).
- **Room:** `VIRTUAL_SHADOW_FIRST_PAIR_ROOM` (262144) a list, grown to the next power of two after a frame finds more (`virtualShadowPairRoomFor`, at most a mesh shader draw's 2^22); a frame past it drops the rest (F4's FULL).
- **Pool:** one `D32_SFLOAT` image, pages in rows of `virtualShadowPoolColumns` (the square root rounded up: 4096 pages, 64 × 64, 8192² texels, 256 MB), made when the pool's size changes (the GPU idle); cleared each frame, depth tested GREATER, both sides, clamped; then readable by fragment shaders.
- **Frame data:** `virtualShadowBuffers` (the pairs' and the casters' list's addresses); the record's `pool` (pages a row, the room, the image's size) and `options.w` (drawn this frame, `markPoolDrawn`). Set 1 binding 61 the pool (a stand-in while not drawn).
- **Debug view:** `showPages` darkens a drawn page where the pool holds a depth 3 of the level's texels sunward of the point (`VIRTUAL_SHADOW_VIEW_BIAS_TEXELS`): a rough look at the pages' shadows, not the main pass's (4c-iii).
- **F4:** `VSM DRAWN` pairs (cut-outs of them), the room, triangles, FULL when a list overflowed.
- **Costs** (`countCosts`, off; 4c-v): the expanding pass adds, a thread its level's once, each level's clusters, pairs and the pairs' triangles (header words 16-63), and the first thread the survivor's: in the camera's culling frustum or not (`setCameraFrustum` puts it in the record: the pass's `views[0]` is the casters' box), kept whole (words 64-72), of them those kept whole for swaying without bones (73-75), and those kept whole for their shape by its first kind: terrain patch, liana, impostor card, following the ground, thinned (80-94). F4: `VSM LEVELS (CLUSTERS:PAIRS:TRIS)` the levels with pairs, `VSM SEEN ... UNSEEN ... WHOLE ... (SWAYING ...)`, `VSM SHAPES (CLUSTERS:PAIRS:TRIS)` by kind.

## Drawing by atomics (4c-iv)

`drawing` = Atomics (the default: 24-39% faster than the depth test at the four spots), with the GPU's `fragmentStoresAndAtomics` (`VulkanDevice::supportsFragmentAtomics`; else the depth test draws): Unreal's way, and what a compute rasteriser (stage 5c) will need. The same expanding pass, pairs and mesh shader; only the target differs.

- **Pool:** an `R32_SFLOAT` image made with a mutable format (`ImageDescription::mutableFormat`), the same size; an `R32_UINT` view of it at set 1 binding 62 (storage, fragment), its own view sampled at 61 as the depth pool is. Remade (the GPU idle) when the drawing changes.
- **Each frame:** cleared by `vkCmdClearColorImage` to the far depth's word, then drawn with no attachments (the pool's size as the render area); `virtual_shadow_atomic.frag` runs `imageAtomicMax` of `virtualShadowAtomicDepth(gl_FragCoord.z)` at its texel, the cut-outs' test first behind its constant (`cut_out_prepass.glsl`). States: `TransferDestination`, `FragmentStorage`, `FragmentRead`.
- **The word:** the reversed depth's float bits, clamped to 0-1 as the depth target clamps, -0 made 0. Bits of floats from 0 up order as the floats, so the largest word is the nearest surface, what GREATER keeps, whatever the order; read through the float view, the bits are the depth.
- **Cost:** a memory atomic for every covered fragment, queued where fragments stack on a texel (crowns about 100); the depth test rejects behind the nearest early. Both timed as VSMDRAW; F4's `VSM DRAWN` line ends `ATOMICS` while drawn so.
- **Measured (home PC, release, VSMDRAW ms, depth test / atomics):** rim 9.3 / 7.0, ring 15.8 / 12.0, wall 24.7-25.3 / 15.0-15.7, trees 17.6 / 11.6, for 36-103 M triangles in 0.6-1.7 M pairs: both far past the cascades' 3.8-5.4 ms.
- A 1 x 1 `R32_UINT` stand-in fills binding 62 while the depth test draws: no shader that runs then reads it.

## The main pass reading the pages (4c-iii)

While `mainPass` (the record's `shading.x`) and the pool is drawn, `sunShadow` (`scene_surface.glsl`) asks `virtualShadowSun` first; cost a page-table read and a pool read a pixel.

1. **Where:** `virtualShadowPointLookup` (shared with the debug view): the sun's shadow point's level by distance, its texel and depth, its page's entry. Not drawn (`virtualShadowEntryDrawn`), past the last level, or a capture (points from another eye): the cascades as before.
2. **Bias:** `virtualShadowBiasDepth`: the cascades' `sunShadowBias` with the level's texel times `ShadowSettings::normalOffsetTexels` (2.5, the record's `shading.y`) as its base; along the light only the depth moves, by that times the depth scale.
3. **Compare:** `virtualShadowLit` against the pool's texel (`virtualShadowStoredDepth`): 0 where the pool holds more than the bias sunward. One texel, hard edges until 4d.
4. **Distant occluders:** the far map's, as a cascade takes them (`withDistantOccluders`).

- Not yet: glass's coloured shadows (the cascades' only), impostor cards (`sunShadowOverPlant` reads the cascades), leaves (not drawn until 4f).

## Tests

`virtual_shadows_test` (the casters' levels at the screen's corner, the casters' box holding every level's corners after walks and nothing a page past; settings and their keys; sizes; the level choice at and a float step past every reach; texels within 0.5-1.1 pixels; points at every level landing half a page inside it, depth inside the range and a caster the reach sunwards too, the worst case straight across the light, cameras 6 km out; walking keeps a world point's slot, texel and depth; which levels drop; slots and table entries each once, negative pages wrapped; depth steps under a quarter texel; marked spans by edges and corners; slots back to pages; entries, sources; the four passes simulated over frames on the CPU with the shared functions: places kept while asked, none stale while walking, a dropped level freed, a full pool evicting then failing, no place held twice; the frame record; the counters and F4's lines); the drawing (drawn entries; a sphere's pages exactly those its square meets, none past the far end; pairs' words at their extremes; the pool's pages each their own square, a texel at the same spot of its page's place; the page's clip distances; the pool's clip space through the flipped viewport; random spheres' pairs holding every drawn page their points land on and no other; the record's pool and the room's growth; F4's DRAWN line); the atomics' word (the far depth's word 0, -0 and negatives 0, past 1 as 1, the bits read back as the depth, order kept, texels struck by 1-100 fragments keeping what GREATER keeps; F4's ATOMICS); the costs (the header's words by level and caster, read only when counted, F4's two lines, counts cut to thousands and tenths of millions); the main pass (the bias the cascades' in the level's texels, a point moved by it changing only its depth by exactly the step; the comparison at and past the bias; no acne on surfaces tilted up to the slope cap at every level, a blocker twice the bias sunward found, half the bias lost; the record's switch and bias); `shadow_culling_test` (the casters' view: the camera's levels, any caster in its box, none outside or not casting); GPU: `virtual_shadows_gpu_test` (level choice exact, pages, slots, entries, keys, sources, colours, edges, level points, spheres' pages, pairs, pool texels and clip spaces, the main pass's bias and comparison, the atomics' word against the mirror); `image_state_test` (the pool cleared, written in fragments, sampled).
