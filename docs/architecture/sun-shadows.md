# Sun shadows

Covers: `src/render/gpu/cascades`, `src/render/gpu/depth_range`, `src/render/vulkan/vulkan_depth_range`, `src/render/gpu/contact_shadows`, `src/render/vulkan/vulkan_contact_shadows`, `assets/shaders/cascades.glsl`, `assets/shaders/depth_range.glsl`, `assets/shaders/depth_range.comp`, `assets/shaders/contact_shadows.glsl`, `assets/shaders/contact_shadows.comp`

The sun's cascaded shadow maps (with the far map, staggering and a mirror's row) and screen-space contact shadows for the sun and nearby lamps. The atlas, its tiles, the shadow cache and lamp shadows are the shadow atlas's topic; mirror rules: `.claude/rules/shader-mirror.md`.

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

## Fit and snapping

- `cascadeSphere(camera, near, far)`: the slice's smallest bounding sphere. Centre on the view axis at `(f + n)(1 + t²) / 2` (t² = the squared half-width plus half-height tangents), or at the far end's centre with radius `f t` when nearer.
- The radius depends only on the lens, so turning never resizes the box (tested with `==`).
- `cascadeBox(direction, sphere, casterReach, tileSize)` → `CascadeBox` {matrix, `depthRange` = 2r + reach, `texelSize` = 2r / tile}:
  - looks along the light, up from `shadowViewUp` (shared with lamp views);
  - centre snapped to whole texels across the light, so edges do not crawl;
  - extends `directionalCasterReach` (200 m) towards the sun so tall casters outside the view still cast.
- Tile edge: `cascadeTilePixels(cascadeResolution)`, 1024, 2048 or 4096 (default 4096; the atlas grows to hold four, see the atlas doc).
- Each cascade's `ShadowTileData`: the box times `translation(camera)` (camera-relative lookup); `offset.y` = `normalOffsetTexels` (2.5) × texel; `lens` = (0, depth range, texel, radius), `lens.x` 0 marking a box rather than a perspective tile.
- Cached like any tile: a box that did not move and whose casters did not change is not redrawn.

Texels for a 70° 16:9 view, unfitted (a box spans about 2.9 × its far distance):

| Cascade ends | 4096 tile | 2048 tile |
|---|---|---|
| ~3 m | 0.22 cm | 0.44 cm |
| ~9 m | 0.65 cm | 1.3 cm |
| ~36 m | 2.5 cm | 5 cm |
| 200 m | 14 cm | 28 cm |

## Lookup

Per cascade, finest first (`sunCascadeRow`):

1. Move the point towards the sun by `sunShadowBias(offset.y, n·l)` = base × (1 + min(tan θ, `MAX_SUN_BIAS_SLOPE` 10)) (`shader_shared.h`).
2. `cascadeEdgeDistance(projected, depthRange / radius)`: 1 at the centre, 0 on any face (sides, near, far), in radii.
3. Take the first cascade with a positive edge distance (by box, not distance: finer boxes often reach past their slice).

- **Why along the light, not the normal:** moving along the rays changes only depth in every cascade, so cascades of different texel sizes agree on where an edge is (`testMovingTowardsTheSunKeepsThePlaceInEveryCascade`).
- **Blend:** `cascadeBlendWeight(edge, cascadeBlendBand)` (0.1 of a radius; 0 switches hard) mixes in the next cascade near the edge; a point the next box misses stays with this one.
- **Dither:** `CascadeTransition::Dither` keeps one cascade per pixel (`cascadeDitherKeeps(weight, interleaved gradient noise)`): one lookup instead of two, grainy until anti-aliasing averages it.
- Band and transition reach the shader in `GpuFrameData::cascadeOptions`.
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

`holdStaggeredCascades` (after `planShadows`), `cascadeStagger` (2; 1 off): turning the camera moves every box and redraws its casters, so the far half of the sun's own cascades take turns.

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

## Contact shadows

Short shadows the maps miss: map texels are millimetres to centimetres and lookups are biased sunwards, so a small object's shadow detaches or vanishes, worst at a grazing sun.

| Part | File |
|---|---|
| Maths, settings, packing, light choice | `render/gpu/contact_shadows` (mirrored by `contact_shadows.glsl`; numbers in `contact_shadows_shared.h`) |
| Pass | `contact_shadows.comp`, `render/vulkan/vulkan_contact_shadows` |
| Main pass reads | `scene_surface.glsl` (`lightContactShadow`) |

### The march

`contactShadowAt` (CPU mirror of the pass's loop), after the depth prepass, one invocation per render pixel in 8 × 8 groups (`CONTACT_GROUP_SIDE`):

- Walks from the pixel's surface towards the light in `steps` (32) even steps over `length` (0.3 m) (`contactStepDistance`).
- Start offset: the blue noise tile's green channel plus `contactNoiseTurn` (golden ratio a frame, `CONTACT_NOISE_FRAME_STEP`, only while anti-aliasing averages it).
- Each step (camera-relative) → jittered view-projection → pixel (`clipToScreen`) and view distance (clip w); the surface there → its view distance (`viewDistanceAtPixel`).
- **Crossing test** (`contactBlocked`): blocked when the step is now behind the surface by more than the bias and less than bias + a step's reach (`contactStepReach` = length / steps), and the previous step was not (the start and passed-over steps count as in front).
  - Bias: `contactBias`, `CONTACT_BIAS_PIXELS` (2) pixels' footprint at the pixel's distance.
  - Why: the depth image shows only fronts; a ray that entered something within the last step is at most one step behind its front, so deeper means passing behind it. A thin thing's shadow is at most a step's reach too wide; no fixed thickness, which doubled post shadows and widened thin ones.
- Mask = 1 − blocked × `contactDistanceFade` (`fadeStart` 40 m to `fadeEnd` 60 m) × `contactEdgeFade` (over `CONTACT_EDGE_PIXELS` 16).
- Sky pixels are lit; a march stops at the screen's edge or nearer than `CONTACT_NEAREST_W`.
- **Cut-outs pass over:** the prepass writes a solid mask (`VulkanRenderer::solidMask`, R8, cleared to 1): the solid pipeline has the attachment with colour writes off (`PipelineDescription::colourWrites`), `cut_out_prepass.frag` writes 0, cut-outs are drawn last so a solid in front stays 1. Steps landing on 0 are skipped; a cut-out's shadow map already draws its holes.

### Point and spot lights

The same pass marches towards up to `localLights` (`CONTACT_MAX_LIGHTS`, 4) lamps.

- `chooseContactLights`: seated lights holding a shadow, ranked by `lightImportance` at the focus; last frame's picks (`VulkanRenderer::contactLightHistory`) count `LightPrioritySettings::incumbentAdvantage` times as bright, so channels do not swap.
- Per pixel, `contactLightRay`: none out of range, outside a spot's cone (`CONTACT_NO_CONE` for a point light) or within the stop; else `length`, or less if the stop is nearer.
- `contactLightStop` = `emitterRadius` + `CONTACT_LIGHT_CLEARANCE` (0.25 m), so the lamp's own glowing mesh in the depth image never shadows it. A shortened march keeps its step count, so its reach shrinks with it.
- `contactShadowTowards` is the CPU mirror; `contactLightFor` builds each `ContactLight` (camera-relative).
- `ContactShadowFrame::runs()` is true for the sun or any lamp, so lamps get contact shadows at night.

### Pass data and the main pass

- `GpuContactShadowPass` (std430, 352 bytes, `packContactShadowPass`): matrices, size, sun direction with the footprint, options (length, steps, reach, noise turn), fades, per lamp position + range, beam + cone, stop, and `lightOptions` (x lamp count, y 1 when the sun marches).
- Sun mask: R32F, scene set 1 binding 31 (`ContactShadowsBinding`; a stand-in when off), multiplying directional light 0's shadow while `GpuFrameData::contactShadowOptions.x` is 1.
- Lamp mask: RGBA16F, a channel each, binding 32 (`LightContactShadowsBinding`), while `contactShadowOptions.y` is 1. A light finds its channel through `GpuFrameData::contactLightSlots` (`contactLightSlotCodes`: point slot, spot slot + `CONTACT_SPOT_SLOT_BASE`, −1 none), so light records are unchanged.
- Not read by probe captures (no depth image of their own) or glass (not in the depth image).
- Timed as `GpuPass::ContactShadows` (CONTACT in F4).

### Settings

`ContactShadowSettings` (`RenderSettings::contactShadows`, F5 Contact shadows, `engine.render.contactShadows.*`): `enabled` (off on Low), `length`, `steps` (1 to `CONTACT_MAX_STEPS` 64; more → tighter round thin things), `fadeStart`, `fadeEnd`, `localLights` (0-4; 0 = sun only). Checked by `validateContactShadows`.

### Limits

- Only what is on screen casts (an off-screen or hidden caster casts nothing).
- Only the shadowed sun and at most 4 lamps (on a street of lamps, the most important at the focus).
- A lamp seen from where it shines (the player's flashlight) casts behind what it lights, so its contact shadows are slivers.

## Tests

| Test | Checks |
|---|---|
| `cascades_test`, `cascades_gpu_test` | Splits, spheres (lens-only radius), snapping, edge distance, blend and dither, bias along the light, the ladder and its rounding down, far map sphere (holds the view, on the grid, still within a cell, one step across one), distant occluders |
| `depth_range_test`, `depth_range_gpu_test` | The range, sky skipped, combining, distances |
| `soft_shadow_test`, `soft_shadow_gpu_test` | Box distances, sun blocker search and penumbra |
| `shadow_map_test` | The row (cascades, far map last, mirror row after every other tile), far map size, smallest caster, movers, distant occluder reach (no gap between a box and the reach), staggering (far half held, matrix following the camera, nothing held when sun, fit, settings or previous plan differ), shrinking order |
| `planar_reflection_test` | The mirrored camera, the mirror row's start on the ladder and never past anything visible |
| `culling_test` | Small casters left out, unknown bounds kept |
| `contact_shadows_test`, `contact_shadows_gpu_test` | Settings and presets, steps, crossing test, bias, fades, noise turn; marches through ray-cast depths (floor and block, cut-outs passed over, a 1 cm pen at 16 and 32 steps, no halo under a hanging bar); lamp rays (range, cone, stop), choosing lights, slot codes, packing |
