# Virtual shadow maps

Covers: `src/render/gpu/virtual_shadows`, `assets/shaders/virtual_shadows.glsl`

The sun's virtual shadow maps (phase 21 stage 4), replacing the cascades step by step; research, choices and the steps: `docs/research/VIRTUALSHADOWMAPS.md`. Built so far: the clipmap's maths (4a). The cascades (`sun-shadows.md`) still draw every shadow.

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

## Tests

`virtual_shadows_test` (settings and their keys; sizes; the level choice at and a float step past every reach; texels within 0.5-1.1 pixels; points at every level landing half a page inside it, depth inside the range and a caster the reach sunwards too, the worst case straight across the light, cameras 6 km out; walking keeps a world point's slot, texel and depth; which levels drop; slots and table entries each once, negative pages wrapped; depth steps under a quarter texel); GPU: `virtual_shadows_gpu_test` (level choice exact, pages, slots, entries, level points against the mirror).
