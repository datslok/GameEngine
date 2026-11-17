# Sky visibility

Covers: `src/render/gpu/sky_visibility`, `src/render/gpu/sky_visibility_bake`, `src/render/gpu/sky_visibility_shared.h`, `src/render/vulkan/vulkan_sky_visibility`, `src/engine/static_geometry`, `assets/shaders/sky_visibility.glsl`, `assets/shaders/scene_sky_visibility.glsl`

How much sky each point round the camera sees, and the light bounced back where it is blocked, so the flat ambient (`sky.md`) dims indoors without light probes or ray tracing. Baked on the CPU against the static level; read like the probes. Its cascades reuse the probes' (`render/gpu/probe_cascades`).

## What a point holds

V(ω) = 1 where a ray escapes, 0 where it is blocked, kept as two spherical harmonic bands: V ≈ a + b·ω (`SkyVisibility`: `open` = a, the open share; `lean` = b, towards the open side).

- `skyVisibilityFromRays`: a = mean opening, b = 3 × mean(opening · ω), directions taken about their own mean (a finite Fibonacci spiral is a little off zero, which would lean a sphere open all round).
- Facing normal n: f(n) = (1/π)∫V max(0, n·ω) dω = a + ⅔ b·n (`skySeenFacing`, clamped 0..1): 1 for a floor under open sky, ½ for a wall in an open field, 0 in a closed box. Along a direction (reflection stand-in): a + b·d (`skySeenAlong`). Air: a.

**Bounce** (`SkyBounce`: three more `SkyVisibility`s, one per colour channel, in `SkyCell::bounce`):

- Each ray meeting a triangle's front brings back that triangle's albedo × the light on the hit, per unit of open-sky light (`skyHitLight`): the sky the hit sees facing its normal + the bounce stored there, read from the grid itself (`sampleSkyVisibility`), so light keeps bouncing as the grid refreshes.
- Where the grid knows less than `skyHitKnownShare` (½) round a hit, it guesses clamp(2a, 0, 1) of the baking point (open ground: half the sphere is sky, the ground sees all of it).
- A surface gets `skyLightFacing` = f + each channel's a + ⅔ b·n, at least `ambientFloor`; `skyLightAlong`, `skyLightInAir` likewise.
- Kept per unit of the flat ambient, which shaders multiply in live, so the time of day needs no re-bake. The sun's own bounce is not carried (the screen's bounce light still gathers it).

## The grid

| | |
|---|---|
| Points a cascade | 48 × 24 × 48 (`SKY_VISIBILITY_ACROSS`, `_HIGH`) |
| Cascades | `cascades` 2 (1 to `SKY_VISIBILITY_MAX_CASCADES` 4), `spacing` 0.5 m doubling each: 24 × 12 × 24 m, then twice that |
| Lattice | fixed in the world, offset `probeLatticeOffsetShare` (½) of the densest spacing so no point sits on a floor at a whole or half metre; a circular buffer, so a move bakes only the planes it brings in |
| Hand-over | `skyVisibilityCascades` widens each cascade's `weightFade` to `cascadeBlend` (0.5) of its reach per axis, never under one spacing; a single spacing showed a line round the camera on lumpy ground |
| Past the last | `skyOpenField` (a = ½, b = ¾ up: sky above, ground below, no bounce): a floor gets the whole flat ambient, a wall and the air half |

`SkyCell`: bands, bounce, reach along ±x, ±y, ±z as a share of the spacing, state (`SkyCellState`: unknown, baked, buried); plus the bake's own notes, not packed: `unsureShare`, `escapedRays`.

**GPU copy** (`VulkanSkyVisibility`): one RGBA32_UINT 3D image, 144 × 24 × 192 (room for four cascades, one after another along z; each point `SKY_CELL_TEXELS` 3 texels along x), read by exact texels, cleared to unknown at start. A point is a `GpuSkyCell` (48 bytes: bands as halves, reaches in bytes, state; then the bounce bands as halves; layout in the header). `packSkyCell` / `unpackSkyCell` round-trip exactly.

## The bake

`bakeSkyCell` (`render/gpu/sky_visibility_bake`), per point:

1. `rays` (256) Fibonacci rays up to `rayLength` (30 m; reaching it counts as open) against the frame's `StaticLevel`.
2. A ray meeting a triangle's back is blocked and votes "inside a wall"; more than `SKY_VISIBILITY_BURIED_SHARE` (¼) makes it buried.
3. Six axis rays up to a spacing give its reach.
4. Front hits gather the bounce (above) when the albedo is given.

- Each thread walks the level's grid with its own `GridMarks`; `skyBakePointsPerJob` (8) points a job.
- A batch's points read the grid as it stood before the batch and are put in place after, so the result is the same on any number of threads.

### Static geometry

`engine/static_geometry` (`StaticGeometry`, owned by `Application`, updated while `skyVisibility.enabled`; into `FrameDescription::staticLevel`, `staticLevelVersion`, `staticAlbedo`):

- `blocksSky`: static, casts shadows (light stand-ins do not, as with `RAY_MASK_SOLID`) and not seen through; or hidden (`ModelRenderer::hidden`: drawn by no view but there, e.g. the forest's walked ground).
- Rebuilt only when `skyBlockersFingerprint` (meshes, matrices, colour, texture, metallic) changes.
- `skyRayCollision`: 1 m cells, only floor-sized triangles on the list every ray tests, which makes rays about twice as fast as the default cells.
- `getAlbedo`: per triangle in level order, linear: sRGB colour × the texture's mean (texels decoded first, cached per texture) × (1 − metallic).

### Queues and stages

`SkyVisibilityGrid`:

- `follow` places the cascades round the camera; points a move brings in wait. A fresh start (first call, new level version, or a changed shape, `sameSkyVisibilityGrid`: spacing, cascades, rays, ray length) makes every point wait and bumps `generation`.
- Waiting points read as not baked: a missing dense point falls to the coarser cascade, past the last to the open field.
- **Order:** coarsest cascade first (a coarse point covers eight dense ones); within one, points ahead (in front of the camera or along the last move) before those behind, nearest first. Per-cascade lists, partially sorted only when not all fit (`takeWaiting`).
- **Stages** (`SkyBakeStage`, two `BakeQueue`s, each in order, each point once, a move clearing a point by its flag):

| Stage | Rays | Who |
|---|---|---|
| Quick | `quickRays` 64 | a fresh grid, or waiting points while behind |
| Refine | `rays` 256 | quick points, and points over `skyUnsureRefineShare` (10%) of whose hits were guessed because the grid round them was unbaked (not hits past the grid) |
| Deepen | `deepRays` 2,048 | full-ray points that saw the sky through 1 to `deepOpenRays` − 1 (31) rays (`skyWantsDeepRays`): few escapes measure the share coarsely (~1/√k). Off at 0 or ≤ `rays` |

Quick, deep rays and the store are not part of the grid's shape: changing them bakes nothing again.

### When and how much

`VulkanRenderer::bakeSkyVisibility` → `VulkanSkyVisibility::bake`, at the start of `render` before the swapchain image is acquired (a long bake then holds nothing of the screen), and in `capture` with `everything`. Only when `readsSkyGrid`: setting on, a static level, no traced probes. CPU stage `SKYBAKE` in F4.

| Case | What bakes |
|---|---|
| Capture (screenshot, golden view) | everything waiting, refined and deepened, at once: the picture must not depend on what came before |
| Fresh grid | every waiting point at once, at the quick rays |
| `backgroundBake` on (default) | the frame applies a finished batch and hands the next to `SkyBackgroundBaker`; never waits |
| `backgroundBake` off | `planSkyBake` within `bakeMilliseconds` (1 ms) of all-core time; behind, at the quick rays (about four times as many); deep rays with what is left |

- **Background:** a batch of about `skyBackgroundBatchSeconds` (30 ms), waiting points first (quick while `quickRays` < `rays`), then refinement, then the deep rays. `SkyBakeBatch` carries each point's slot, cascade and lattice place and a shared copy of the grid for its hits (`batchGridNow` updates the kept copy with only the changed points). `applyBatch` puts a point in its slot while the slot still holds its place, else only into the store; drops the batch if `generation` moved. The synchronous `bake`, `refine`, `deepen` take the same path.
- **Baker threads:** a `JobSystem` of its own at low priority (`SDL_SetCurrentThreadPriority` through the worker start hook) with `skyBackgroundWorkers(backgroundShare)` threads (half the hardware threads), remade when the share changes. On the frame's job system the frame thread, waiting for its own jobs, ran bake jobs too.
- **Budget's cost:** seconds a point at the full rays, measured only on batches of at least `skyMinimumBatch` (a round of jobs per core; smaller ones run on fewer cores and would shrink each next budget until the bake crawled), quick and deep batches scaled by their rays (`skySecondsAtFullRays`). `skyCellsForBudget` turns it into a count.

### The store

`SkyCellStore` (`storeMegabytes` 256, 0 off): every baked point also kept by cascade and lattice coordinate (`probeLatticeCoord`), packed as the GPU reads it, with its next stage.

- A slot a move brings to a kept place takes it at once (`arrive`); one still needing refinement or deep rays rejoins that queue. A point depends only on the level and its place, so a kept one is exact.
- Least recently used out first (a list and a map, O(1)); a slot leaving a place counts as a use. About `skyStoreBytesPerPoint` (128) a point: 256 MB ≈ 2.1 million points.
- Places past ±2^19 lattice steps on an axis are not kept. A new level or shape empties it.

### Upload

`VulkanSkyVisibility::record`, in the frame's commands before the fog and main pass read the image, only when the grid's version moved: repacks only `takeChanges`' points into a whole packed copy, copied through the frame in flight's own staging buffer.

## Reading it

`skyVisibilityAt` (`scene_sky_visibility.glsl`), mirrored by `sampleSkyVisibility`:

- Cascades densest first, each adding its blend × its weight round the camera × what denser ones left (as `sampleProbeCascades`).
- Within one, the eight points round the lookup point, moved off the surface by `lookupBias` (0.3) of the probes' self-shadow offset, weighted by nearness (`probeTrilinearWeight`), side (`probeBackFaceWeight`, crushed) and kept only where `skyCornerSees`: the offset lies within the point's reach on each axis. Walls mostly stand along the axes, so this is a cheap stand-in for the probes' distance test against leaks.
- Unbaked and buried points are left out; a cascade with none left adds nothing and its share is `missing`. The remainder reads as the open field.
- Air (`skyVisibilityInAirAt`): the `missing` share left dark instead (`skyMissingLeftDark`), since the fog's jittered samples landing in a wall would flip between indoor and outdoor light; past the grid stays open field.

Set 1 binding 29 (the image), binding 30 (the cascades' `GpuProbeCascade` records, camera-relative). `GpuSkyVisibilityOptions` in the frame block: cascades read (0 = off), floor, lookup bias, debug view.

**Readers** (only while `readsSkyGrid`):

| Where | What |
|---|---|
| Main pass (`scene_surface.glsl`) | diffuse light from all around `flatAmbient() × skyLightFacing`; the reflection stand-in along the mirror direction; the back side for light through leaves |
| Fog (`fog_light.comp`) | the air's share |
| Sun past its cascades (`sunBeyondCascades`, without ray tracing) | `skySeenAlong` towards the sun stands in for the shadow, so the reflection probe's capture behind the camera keeps rooms dark; beyond the grid the open field leaves far ground sunlit |
| `debugView` | the facing share (sky + bounce, per channel) as the colour; the reflection pass kept out of it |

Command: `sky-points <x y z>` (`describePoints`): each cascade's eight points round a place, their state, queue, open share, unsure share and bounce.

## Settings

`SkyVisibilitySettings` (`RenderSettings::skyVisibility`, F5 Sky visibility, keys `engine.render.skyVisibility.*`); meanings and defaults in the header. Off on the Low preset. Live without a re-bake: `ambientFloor`, `cascadeBlend`, `lookupBias`, `bakeMilliseconds`, `backgroundBake`, `backgroundShare`, `quickRays`, `deepRays`, `deepOpenRays`, `storeMegabytes`, `debugView`. The rest reshape the grid and bake it afresh. `validateSkyVisibility` throws on bad values.

## Tests

- `sky_visibility_test`: bands (open field, closed box, open sphere), shares and clamps, floor, reach, packing, cascades; reading with nothing baked, uniform points, buried and walled points left out; open field past the grid, air in a wall dark; settings, keys, presets; baking over a floor, under a roof past the ray length, in a room with a door; the bounce's analytic floor (albedo ρ under a uniform sky: ρ facing down, ½ρ sideways, within 0.03); bake plan, budget, minimum batch, following a plane at a time, fresh starts; queue order, refinement, unsure points, deep rays (who wants them, error against a far deeper bake); store (keep, find, LRU, exact repack); batches and the background baker; one thread vs four.
- `static_geometry_test`; `uniform_grid_test` (per-thread marks); `half_float_test`.
- GPU: `sky_visibility_gpu_test`.

## Limits

- Only the static level counts: a door opening or a crate moving changes nothing.
- The bounce is one colour per triangle and per unit of sky light; no sunlight bounce.
