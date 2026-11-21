# Sun shadows

Covers: `src/render/gpu/cascades`, `src/render/gpu/depth_range`, `src/render/vulkan/vulkan_depth_range`, `assets/shaders/cascades.glsl`, `assets/shaders/depth_range.glsl`, `assets/shaders/depth_range.comp`

The sun's cascaded shadow maps (with the far map, staggering and a mirror's row). Contact shadows: `contact-shadows.md`. The atlas, its tiles, the shadow cache and lamp shadows are the shadow atlas's topic; mirror rules: `.claude/rules/shader-mirror.md`.

## Who gets cascades

- The brightest directional light only (`maxShadowedDirectionalLights` = 1; slot 0 after `prioritizeLights`).
- `ShadowSettings` (`render/gpu/shadow_map.h`, F5 Shadows, `engine.render.shadows.*`) holds every knob below; meanings in the header.
- `planShadows` builds the sun's row through `addSunCascades` (shared by the camera's and the mirror's rows), finest first, tiles in a row from `LightShadow::firstTile`; `LightShadow::cascades` counts them (the far map included).
- `emitterRadius` = tan of `DirectionalLight::angularRadius` (0.00465 rad, the sun's), for the soft edge.

| File | Role |
|---|---|
| `render/gpu/cascades` | Splits, spheres, boxes, the fit's ladder, the far map's sphere, bias, blend, distant occluders; mirrored by `cascades.glsl` |
| `render/gpu/shadow_map` | `ShadowSettings`, `planShadows`, `holdStaggeredCascades`, `LightShadow`, `ShadowTileData` |
| `render/gpu/soft_shadow` | Box distances and the sun's blocker search and penumbra |
| `render/gpu/depth_range`, `render/vulkan/vulkan_depth_range`, `depth_range.comp` | Measuring the screen's farthest surface |
| `scene_surface.glsl` | `sunShadow`, `sunCascadeRow`, `sunTileShadow`, `sunRowOrLit` |

## Splits

- `cascadeSplits(near, far, count, lambda)`: practical splits, `lambda × logarithmic + (1 − lambda) × even`, ends exact.
- `cascadeCount` 4 (1 to `maxShadowCascades`), `cascadeSplitLambda` 0.95.
- Near end: the camera's near plane. Far end: the least of `directionalDistance` (200 m), the far plane and the fitted distance (below).
- Unfitted, a 70° 16:9 view splits at about 3, 9, 36 and 200 m.

## Balls and steps

Research: `docs/research/SHADOWCACHE.md` (Cascades while the camera moves).

- `cascadeBall(camera, depth, radius)`: a cascade ending at split `depth` is the ball round the camera of `radius` (`depth`, the last cascade's below): turning never moves it.
  - A narrow view (under 90° across the diagonal, a scope) takes `cascadeSphere(camera, 0, depth)` instead, the smallest sphere round the view up to depth (centre on the axis at `f(1 + t²) / 2`, t² = the squared half-width plus half-height tangents, or the far end's centre with radius `f t`), smaller than the ball there.
  - A view point within `depth` of the camera is in the ball; further, in a corner, the next cascade's (chosen by box).
- **The shadow distance is a radius:** cascade shadows end `directionalDistance` from the camera every way; at the view's corners, past it, the far map shades.
- **Last cascade** (`lastCascadeRadius`): fitted to a depth (a room), its radius is the depth × `viewCornerStretch` (√(1 + t²), 1.74 at 70° 16:9, capped at the shadow distance), so the fitted view's corners stay covered.
- `cascadeBox(direction, sphere, casterReach, tileSize, stepTexels)` → `CascadeBox` {matrix, `depthRange` = 2h + reach, `texelSize` = 2h / tile, `halfWidth` h}:
  - looks along the light, up from `shadowViewUp` (shared with lamp views);
  - centre the sphere's rounded to steps of `stepTexels` texels on all three light axes, half-width h = r · tile / (tile − k), so the ball fits wherever it is in its step; k = `cascadeStepTexels(cascadeStepShare` 1/16, tile) = 256 at 4096;
  - k = 0 (the far map; a share of 0): whole texels across only, h = r;
  - built round the origin then translated, so a moved box keeps its upper 3 × 3 bit for bit;
  - extends `directionalCasterReach` (200 m) towards the sun so tall casters outside the view still cast.
- A step is k whole texels across and the same length in depth, so a cascade moves only when the camera crosses a step (`testWalkingRarelyMovesTheSunCascades`), its edges never crawl, and a scrolled copy would line up.
- Tile edge: `cascadeTilePixels(cascadeResolution)`, 1024, 2048 or 4096 (default 4096; the atlas grows to hold four, see the atlas doc).
- Each cascade's `ShadowTileData`: the box times `translation(camera)` (camera-relative lookup); `offset.y` = `normalOffsetTexels` (2.5) × texel; `lens` = (0, depth range, texel, h), `lens.x` 0 marking a box rather than a perspective tile.
- Cached like any tile: a box that did not move and whose casters did not change is not redrawn.

Texels for a 70° 16:9 view, unfitted (a box spans 2 × 16/15 × its far distance):

| Cascade ends | 4096 tile | 2048 tile |
|---|---|---|
| ~3 m | 0.16 cm | 0.31 cm |
| ~9 m | 0.47 cm | 0.94 cm |
| ~36 m | 1.9 cm | 3.8 cm |
| 200 m | 10.4 cm | 21 cm |

## Lookup

Per cascade, finest first (`sunCascadeRow`):

1. Move the point towards the sun by `sunShadowBias(offset.y, n·l)` = base × (1 + min(tan θ, `MAX_SUN_BIAS_SLOPE` 10)) (`shader_shared.h`).
2. `cascadeEdgeDistance(projected, depthRange / radius)`: 1 at the centre, 0 on any face (sides, near, far), in radii.
3. Take the first cascade with a positive edge distance (by box, not distance: finer boxes often reach past their slice).

- **Why along the light, not the normal:** moving along the rays changes only depth in every cascade, so cascades of different texel sizes agree on where an edge is (`testMovingTowardsTheSunKeepsThePlaceInEveryCascade`).
- **Blend:** `cascadeBlendWeight(edge, cascadeBlendBand)` (0.1 of a radius; 0 switches hard) mixes in the next cascade near the edge; a point the next box misses stays with this one.
- **Limited casters thin out:** a caster limited to the nearest cascades (`shadowCascades`) would stop at its last cascade's edge. There `shadowThinKeeps` (`camera_culling`, in `shadowTakes`) keeps a share falling from 1 at `limitedCasterThinStart` (0.5 of a radius from the sides) to 0 at the blend band, by each caster's fixed PCG3D number of its centre. Culled, not drawn; the band is in `ShadowTile::thinStart`/`thinEnd` and the cache key.
- **Dither:** `CascadeTransition::Dither` keeps one cascade per pixel (`cascadeDitherKeeps(weight, interleaved gradient noise)`): one lookup instead of two, grainy until anti-aliasing averages it.
- Band and transition reach the shader in `GpuFrameData::cascadeOptions` (xy; z is impostor cards' shadow spread, `level-of-detail.md`).
- Impostor cards read the sun's cascades once a plant, averaged over points round it (`sunShadowOverPlant`, `level-of-detail.md`).
- `sunCascadeRow` returns its shadow and `ownShare`; `sunShadow` fills the rest from the mirror's row when there is one, else full light (`sunRowOrLit`).

### Soft edges

`softShadowFromTile` handles boxes (`render/gpu/soft_shadow`):

- distance from the box's near side: `boxShadowDistance(depth, depthRange)`;
- blocker search: `sunBlockerSearchTexels(tan, receiver, texel)` (a blocker at the near side);
- penumbra: `sunPenumbraTexels` = tan × (receiver − blocker) in texels, clamped to `MIN_SHADOW_FILTER_TEXELS`..`MAX_SHADOW_FILTER_TEXELS` (1-12).
- Worked out in metres, then converted per cascade, so widths match across a seam.

## Fitted to the screen

Sample distribution shadow maps (Lauritzen 2011), `fitCascadesToDepth` (on): the cascades end at the farthest surface on screen. Only the far end is fitted; moving the start to the nearest surface would push every split out and coarsen the nearest pixels.

### Measurement

- `depth_range.comp` (`GpuPass::DepthRange`, DEPTHS in F4) reads the depth image after the main pass (`drawMainPass(..., keepDepth, ...)` stores it; the image goes to `ImageState::DepthComputeRead`).
- Each invocation reads a 2 × 2 block (`DEPTH_RANGE_TEXELS_PER_SIDE`) in 16 × 16 groups (`DEPTH_RANGE_GROUP_SIDE`), keeping `ScreenDepthRange` {farthest bits, nearest bits}.
- Reversed-Z: the farthest surface has the smallest depth; the far depth 0 is sky and skipped (`isSurfaceDepth`).
- Positive floats' bits sort like the values, so `subgroupMin`/`subgroupMax`, shared-memory atomics and two global atomics per group combine in any order.
- `VulkanDepthRange`: the pipeline (range buffer at binding 0, depth sampled at 1), the range reset by `vkCmdUpdateBuffer`, copied to a read-back buffer per frame in flight. `measure` records; `collect(frameIndex)` returns it once after that frame's fence, so the CPU fits a frame or two late (`VulkanRenderer::unusedDepthRange`).
- To distance: `farthestSurfaceDistance(range, near, far)` (`linearShadowDepth`).

### The ladder

- `ShadowDistanceFit {optional step}`: distance = `shadowDistanceStep` (1.25) ^ step metres.
- `fitShadowDistance(previous, farthest)`: the step reaching `farthest × shadowDistanceMargin` (1.1, for the lag). Climbs at once; comes down only when the need is `shadowDistanceShrinkSteps` (2) lower, so boxes keep one size (and stay cached) while the view wobbles. No surface (all sky) → unfitted.
- `VulkanRenderer::updateSunShadowFit` applies new measurements (sun and mirror); fitting off resets both fits and drops stale measurements.
- `fittedShadowDistance` (infinity unfitted) → `planShadows(..., fittedSunDistance)`. `RenderStats::sunShadowDistance` shows it (SUN TO on F4's tile line).
- Golden captures: `drawFramesBeforeCapture` draws the same frame first to measure it, then restores the light history, the contact lights' history, the atlas layout and the shadow cache, so a capture is still a first frame plus its measurement.

## The far shadow map

One more box at the end of the sun's row (`ShadowKind::Far`, right after the cascades), so mountains and towers kilometres away cast. The row walk reads it as the last cascade, blending in from the last cascade's band and fading to lit over its own.

- **Sphere:** `farShadowSphere(camera, distance)`: centre snapped to a grid `farShadowSnapShare` (1/8) of the distance apart (`farShadowSnap`), radius the distance plus half the cell's diagonal. It stays put while the camera walks inside a cell, so the box and its cached tile do too.
- **Distance:** `farShadowDistance` 5000 m (0 off), at most the far plane; none when the view ends before the cascades do (`farShadowReach`).
- **Tile:** `farShadowResolution` (4096), at most the atlas's largest square: 4096 on Ultra (~2.7 m texels over 5 km), 2048 otherwise. Gives way first when the atlas is short, down to 1024, before any lamp shrinks.
- **Why metres-wide texels suffice:** the sun's half-degree disc blurs a shadow edge by about 1/100 of the caster-to-ground distance (2.8 m under a top 300 m up).
- **Small casters left out:** `ShadowTile::smallestCaster` = `farShadowSmallestCaster` (2 texels), applied by `castersAtLeast` (`scene/culling`) in `prepareShadowDraws`, signature included; unknown bounds kept. Keeps the player's body from redrawing kilometres of land each frame.
- **Movers:** the far tile sets `ShadowTile::separateMovers`: still casters in their own copy, movers drawn over copied-back patches (the shadow cache's topic).
- **Redrawn** only when the camera changes cell, the sun turns or a big caster changes.
- **Bias:** the cascades' (`normalOffsetTexels` of its own texel), so its shadows start a few metres from their casters' feet.

### Distant occluders

`distantOccluders` (on): cascades take casters only `directionalCasterReach` towards the sun, so casters hundreds of metres up are missing from them.

- Where a cascade shades a point, also read the far tile from `distantOccluderPoint(position, toLight, reach)`, the unbiased point moved the reach sunwards: only its depth changes, so only blockers beyond the reach count, never the far map's own coarse self-shadowing.
- Combined by `withDistantOccluders`: the darker of the two per channel, not the product (a caster can be in both).
- Readers: the main pass and reflection probe capture (`sunTileShadow`, soft via `softShadowFromTileLifted`, its penumbra adding the reach back), the fog (`sunShadowInAir`, one comparison through `shadowTap`).
- Skipped where the cascade is already fully dark, inside the far tile itself, and without a far map.
- Reaches the shader as `GpuDirectionalLight::farShadowTile` and `distantOccluderReach` (from `LightShadow::farTile`, `distantReach`).

## Staggered cascades

`holdStaggeredCascades` (after `planShadows`), `cascadeStagger` (2; 1 off): a box that moves redraws its casters, so the far half of the sun's own cascades take turns. Since the boxes are balls in steps, only a step crossed (or a narrow view turning) moves one.

- Cascade k of the far half is redrawn on frames where frame + k is a multiple of the stagger; with two far cascades and 2, one a frame, so every frame costs the same.
- Otherwise it keeps last frame's tile from `VulkanRenderer::lastShadowPlan` (box, square, reach, record); only the shader's matrix is rebuilt from the camera's position, so its shadows stay put.
- Kept only with the same square and the same box shape (the matrix's upper 3 × 3: sun direction, size, depth); a turning sun, a new fit or new settings redraw it.
- Never held: the near half, the far map, a mirror's row.
- Cost: a held cascade's edge lags a frame behind a turning view (the next cascade or the far map covers it); movers in it update at its rate.

## A mirror's cascades

A flat mirror's picture (planar reflections) shows mostly what is behind the camera, outside the camera's boxes. With a mirror, `planShadows` takes a `MirrorShadowView` and the sun gets a second row (`ShadowKind::Mirror`).

- **View:** `mirroredCamera(camera, plane)`, same lens, reflected; its slices built by `addSunCascades` as the camera's.
- **Start:** `mirrorCascadesStart`: the mirrored camera's distance to the glass times the cosine of the view's corner angle (the nearest anything past the glass can be), rounded down the ladder (`shadowLadderStepBelow`) so boxes do not creep; never before the near plane.
- **End:** the mirror picture's farthest surface, measured by a second `VulkanDepthRange` (`mirrorDepthRange`) and fitted on the same ladder (`mirrorShadowFit`); depths convert with the camera's planes (the picture uses its reflected projection). Capped by the shadow distance and the far plane.
- **Settings:** `mirrorCascadeCount` (2, 0-4; 0 leaves the mirror in full sun), `mirrorCascadeResolution` (2048, at most the largest square).
- **Tiles:** after every other tile, the first to shrink when the atlas is full. `LightShadow::mirrorFirstTile` and `mirrorCascades` reach the shader in `GpuDirectionalLight`.
- **Lookup:** the camera's row first; where no box holds the point, or in its last cascade's band, the mirror's row takes over, so the camera's last cascade fades into the mirror's rather than into sunlight.

## Tests

| Test | Checks |
|---|---|
| `cascades_test`, `cascades_gpu_test` | Splits, spheres (lens-only radius), balls (round the camera, a scope's cone, the view within depth held), corner stretch, steps (box half a step wider, holds its ball, moves in whole steps only), snapping, edge distance, blend and dither, bias along the light, the ladder and its rounding down, far map sphere (holds the view, on the grid, still within a cell, one step across one), distant occluders |
| `depth_range_test`, `depth_range_gpu_test` | The range, sky skipped, combining, distances |
| `soft_shadow_test`, `soft_shadow_gpu_test` | Box distances, sun blocker search and penumbra |
| `shadow_map_test` | The row (cascades, far map last, mirror row after every other tile), the view covered within the shadow distance (a fitted view's corners too), turning moving no cascade, walking a metre moving each a few times, far map size, smallest caster, movers, distant occluder reach (no gap between a box and the reach), staggering (far half held, matrix following the camera, nothing held when sun, fit, settings or previous plan differ), shrinking order |
| `planar_reflection_test` | The mirrored camera, the mirror row's start on the ladder and never past anything visible |
| `culling_test` | Small casters left out, unknown bounds kept |
