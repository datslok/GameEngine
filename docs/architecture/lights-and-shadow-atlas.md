# Lights and the shadow atlas

Covers: `src/render/gpu/light_priority`, `src/render/gpu/shadow_map`, `src/render/gpu/shadow_atlas`, `src/render/gpu/shadow_cache`, `src/render/gpu/soft_shadow`, `src/render/gpu/shader_shared.h`, `assets/shaders/soft_shadow.glsl`, `assets/shaders/scene_shadow_tiles.glsl`, `assets/shaders/shadow.vert`, `assets/shaders/shadow.frag`, `assets/shaders/shadow_cut_out.vert`

Which lights the shader gets, which cast shadows, and how point and spot shadows are planned, cached, drawn and filtered. The sun's cascades, far map and contact shadows: `sun-shadows.md`. Glass tinting shadows: `glass.md`. Research on caching: `docs/research/SHADOWCACHE.md`.

## Light priority (`prioritizeLights`)

`drawScene` calls it before packing, with the frame's lighting, `FrameDescription::lightFocus`, the camera frustum, last frame's `LightHistory`, `frameSeconds` and `RenderSettings::lights` (`LightPrioritySettings`).

1. Drop lights whose range sphere misses the frustum.
2. Rank by `priority` (an int on `PointLight` and `SpotLight`; higher always wins), then by `lightImportance` at the focus: intensity × brightest channel / (d² + sourceRadius²). No range window, so far lights still rank by distance instead of tying at zero. Directional lights by brightness.
3. Seat the top lights up to the seats (default every slot the buffers hold: clustered shading makes a light cost only where it reaches).
4. Give shadows to the top seated casters up to the shadow caps, switching `castsShadows` off on the others' copies, so `planShadows` needs no special case.
5. Return the lights, the new `LightHistory` (entities in view; each holder's `LightFade`: light and shadow strength 0..1) and `pointLightsInView` for the readout.

**Settings** (`LightPrioritySettings`, header for meanings): `pointSeats`, `spotSeats` (default `defaultPointSeats` 1024, `defaultSpotSeats` 256), `shadowedPoints`, `shadowedSpots` (default `maxShadowedPointLights` 64, `maxShadowedSpotLights` 8), all clamped to their limits; `incumbentAdvantage` 1.25; `fadeSeconds` 0.25.

**Hysteresis:** last frame's holders count `incumbentAdvantage` times as important, between equal priorities only, so similar lights do not swap as the focus moves.

**Handovers** (no pops, limits always held):

- The losing holder keeps its seat or shadow while fading out over `fadeSeconds`, freeing it at zero; only then does the winner fade in.
- A light not on screen last frame (just switched on, just in view) starts at full strength: nothing showed it before.
- A light leaving the view is dropped at once.
- Strengths reach the GPU as `PlacedPointLight`/`PlacedSpotLight::fade` (multiplied into radiance by `packLighting`) and `shadowFade` (`LightShadow::strength`, copied by `attachShadows`; the shader mixes the shadow towards lit).
- Lights are recognised by `Entity`; `Entity{}` never counts as last frame's.

**Focus:** `Application` sets `lightFocus` from `Game::getLightFocus()` (default the camera). The demo returns the MOBA camera's look point in MOBA mode.

The shader skips a light, and its shadow lookups, for pixels beyond its range.

## Which draws cast

- `ModelRenderer::castsShadows` (default true, on each `DrawItem`) decides what blocks light; a light's own `castsShadows` decides whether it casts.
- `shadowOnly`: drawn into shadow maps only, skipped by every camera view (`visibleDraws`) and met by no ray (`rayMaskFor` gives mask 0, keeping its instance slot), so a cheap stand-in can cast for something whose own `castsShadows` is off (the forest's leaves).
- `shadowCascades`: limits a caster to the sun's nearest N cascades (0 all); `castersForCascade` filters by `ShadowTile::sunCascade`, and such casters thin out across their last cascade (`sun-shadows.md`).

## The atlas (`shadow_atlas`)

One D32 depth image for every shadow view, so the shadow pass is one render pass and the shader binds one texture (one 2D image, since SDL_GPU could not draw into a layer of an array; kept through the port).

- `ShadowAtlasSize`: 8192 × 4096 (128 MB, largest square 2048) by default; with 4096 cascades `shadowAtlasSizeFor` grows it by the cascades' rows to 8192 × 12288 (384 MB, largest square 4096). `ensureShadowAtlas` remakes it (and redraws everything) when the size changes; cleared to `gpuFarDepth` at creation.
- Tile sizes: powers of two from `smallestShadowTileSize` 256; `standardShadowTileSize` 1024 (point faces' largest); `largestShadowTileSize` 2048 (spotlights').
- `ShadowAtlasAllocator`: a buddy allocator on a quadtree; the atlas starts as its largest squares, a request takes the smallest free square that fits, splitting quarters; a freed square rejoins its siblings when all four are free.
- `ShadowAtlasLayout::place`: each view asks by `ShadowTileKey` (kind, entity or slot when the entity is `Entity{}`, face) and gets last frame's square back for the same size; new ones placed largest first; if fragmentation leaves no room, everything is repacked once (`repackedLastTime`). The two-argument `planShadows` uses a fresh layout.

## Planning (`planShadows`)

`planShadows(lighting, camera, layout, settings, fittedSunDistance, mirror)` hands out tiles in the slots `packLighting` uses.

| Tiles, in order | Count | View |
|---|---|---|
| The brightest directional light (`maxShadowedDirectionalLights` 1) | up to `maxShadowCascades` (4) boxes, then the far map | `sun-shadows.md` |
| Each shadowed spotlight | 1 | perspective down the beam (`spotShadowMatrix`) |
| Each shadowed point light | 6 | cube faces, +X -X +Y -Y +Z -Z (`pointShadowFaceMatrix`; `pointShadowFace` = largest axis, mirrored in the shader) |
| A flat mirror's sun cascades | `mirrorCascadeCount` (2) | shadows and reflections docs |

- `maxShadowTiles` = 64 × 6 + 8 + 9 = 401. Lights past the caps still light, unshadowed.
- A point face whose widest view misses the camera frustum (`mayIntersect`) gets no square and no draw: size 0, empty rect, which the shader counts as lit, keeping `firstTile + face` valid.
- **Size:** `shadowTileSizeFor(halfWidth, range, texelTarget, largestSize)`: the power of two whose texel covers about `texelTarget` across the far end of the view. Point faces use `ShadowSettings::texelTarget` (0.04) up to 1024; spotlights `spotTexelTarget` (0.02, finer: beams land at a slant short of their reach) up to 2048.
- **Fitting:** `shrinkToFit` halves the least important requests while the total overflows: the far map first (down to 1024), then spots and points from the last slot back (to 256), then the sun's cascades (never below 1024). Mirror cascades come last in the list, so they shrink before any point light. Two `static_assert`s prove everything fits at its smallest, for both atlas sizes.
- Perspective views are widened by a 2-texel margin (`edgeMarginTexels`) so filtering never leaves the tile; matrices therefore depend on tile size.
- **Output** (`ShadowPlan`): `tiles` (`ShadowTile`: matrix, square, `reach`, clip plane, light place, `sunCascade`, `separateMovers`, `smallestCaster`), `tileData` (`ShadowTileData` for the shader), and one `LightShadow` per light in slot order (`directional`, `spots`, `points`: first tile or -1, strength, emitter radius, cascade counts, mirror and far tiles). `attachShadows(GpuLights&, plan)` copies them into the light records.
- `ShadowTile::reach`: the light's range sphere (radius negative for the sun, which reaches everywhere).

## Shader data

`ShadowTileData` (112 bytes, set 1 binding 6, `scene_shadow_tiles.glsl`):

- `matrix`: the tile's view for camera-relative points (`depth.md`).
- `offset`: x normal offset per unit of distance (perspective) or y fixed (boxes); z 1 when its coloured shadow map holds glass (`markTileGlass`).
- `rect`: the square in the atlas, 0..1.
- `lens`: perspective: near, far, texel size per unit of distance; box: 0, depth, texel size, radius (x 0 tells them apart).

Bindings 7 and 8: the atlas with the comparison sampler (`GREATER_OR_EQUAL`, linear, reversed-Z) and raw (nearest, for blocker searches).

## Caching (`shadow_cache`)

A tile's image depends only on its view and its casters, so unchanged tiles are not redrawn.

- `ShadowTileMemory`: per square, the matrix and signature last drawn there. `isCurrent`, `remember` (forgets squares the new one overlaps), `forgetAll` (after any render error, or atlas contents lost).
- `shadowCasterSignature`: FNV-1a over each caster's mesh, model matrix, shape and, with levels, the level drawn; `tintedShadowSignature` adds the glass that tints it (identical without glass); `swayingShadowSignature` (`render/gpu/wind`) makes a swaying tile redraw every frame.
- **What the fingerprint covers:** only the moving draws and glass; still changes come from the scene's announcements and the GPU finds the casters (`gpu-driven.md`, Shadow tiles).
- **Still and moving casters** (`separateMovers`): its still casters cached in a "Still shadow" image of its own (`VulkanRenderer::stillShadows`, by the tile's square); each frame the movers' patches (`shadowPatch`, `splitMovingCasters`, `movedSinceLastFrame`) are copied back from it and the movers drawn over them.
- **Which tiles** (`keepsMoversApart`, set each frame before the tile's needs): the far map always; a sun cascade once a moving draw is in it, then while it has its copy (a new copy redraws the tile); never a swaying cascade or a lamp's tile. A drifting rock then redraws its patch, not the forest under the whole cascade. Cost: a copy the tile's size (64 MB at 4096) for each cascade a mover reached.

## Preparing and drawing

`prepareShadowDraws` (CPU, before recording; `gpu-driven.md`, Shadow tiles):

1. Skip size-0 tiles.
2. The tile's needs (`shadowTileNeeds`): its key (moving draws it takes, glass, levels, sway) against `ShadowTileMemory`, and the still changes touching it; unchanged tiles without movers are counted cached and skipped.
3. A drawn tile: a view of the GPU culling, room for its lists in the draw list, its glass batched there by the CPU.

The CPU's reference for a tile's casters (`shadowTileCasters`: `castersInReach`, `shadowCasters` for its view, `castersAtLeast(smallestCaster)`, `castersForCascade`, levels by `tileLevelView`) is what the tests and `cull-compare` check the GPU against.

`drawShadows`:

- Separate-movers tiles first: still copy redrawn if needed, patches copied into the atlas.
- Atlas `DepthTarget`, loaded and stored (only some tiles change); per tile: viewport, `shadowClearPipeline` (`fullscreen_triangle.vert` at `gpuFarDepth`, depth `ALWAYS`) unless it keeps movers apart, `GpuShadowView` pushed, casters drawn; then `DepthFragmentRead`. No pass at all when nothing changed.
- Pipelines: `shadow.vert`/`shadow.frag` (positions only) and for cut-outs `shadow_cut_out.vert`/`cut_out.frag` (holes thrown away), both reading instances through `drawnObject` so placed plants' entries draw too (`planting.md`), and bending lianas' template tubes onto their records, their ivy's cards in the cut-out pipeline (`lianaDrawnVertex`, `liana_data.glsl`, wind.md); depth `GREATER`, no culling (`VK_CULL_MODE_NONE`), no hardware depth bias (its slope-scaled part detached shadows from pillars seen edge-on by small tiles).
- Coloured shadow maps after, when glass needs them (`drawGlassShadows`, `glass.md`).

## Lookups and soft shadows

In `scene_surface.glsl`:

- `offsetForShadow`: the point moves along its normal by `ShadowSettings::normalOffsetTexels` (2.5) texels of its tile, the only bias. The sun's lookups move towards the sun instead (`sun-shadows.md`).
- Points outside a tile's view count as lit.
- Lights without an emitter size use a plain 3 × 3 filter (`shadowFromTile`).

**PCSS** (`softShadowFromTile`, arithmetic in `soft_shadow.glsl`, mirrored in `render/gpu/soft_shadow`, tested in `soft_shadow_test` and on the GPU):

1. Blocker search: 16 Poisson-disc samples of the raw depths within `blockerSearchTexels` (up to `MAX_BLOCKER_SEARCH_TEXELS` 12), averaging the stored depths in front of the receiver (`linearShadowDepth`).
2. Penumbra: `penumbraTexels` = emitterRadius × (receiver − blocker) / blocker, over the receiver's texel size, clamped 1 to 12 (`MIN_SHADOW_FILTER_TEXELS`, `MAX_SHADOW_FILTER_TEXELS` in `shader_shared.h`).
3. 16 comparison samples over it, turned per pixel by interleaved gradient noise, clamped a texel inside the tile, each compared against the receiver's plane (receiver plane depth bias: the plane's slope in the tile from the camera-relative position's screen steps, taken at the start of `main` in uniform control flow).
4. With glass in the tile, each sample also takes the nearer glass and its tint.

Emitter sizes: `PointLight::emitterRadius` 0.05, `SpotLight::emitterRadius` 0.025: the glowing size for shadows, separate from `sourceRadius`, which only shapes the falloff. The sun's box variant (`sunBlockerSearchTexels`, `sunPenumbraTexels`) uses its angular radius: `sun-shadows.md`.
