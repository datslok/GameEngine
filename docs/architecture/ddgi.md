# Light probes (DDGI)

Covers: `src/render/gpu/ddgi`, `src/render/gpu/ddgi_shared.h`, `src/render/gpu/ddgi_sampling`, `src/render/vulkan/vulkan_ddgi`, `assets/shaders/ddgi.glsl`, `assets/shaders/ddgi_sampling.glsl`, `assets/shaders/ddgi_volume.glsl`, `assets/shaders/ddgi_update.glsl`, `assets/shaders/ddgi_trace.comp`, `assets/shaders/ddgi_mirror.comp`, `assets/shaders/ddgi_seed.comp`, `assets/shaders/ddgi_share.comp`, `assets/shaders/ddgi_update_irradiance.comp`, `assets/shaders/ddgi_update_distance.comp`, `assets/shaders/ddgi_probes.vert`, `assets/shaders/ddgi_probes.frag`, `assets/shaders/scene_probes.glsl`, `assets/shaders/scene_probe_states.glsl`, `assets/shaders/scene_probe_cascades.glsl`, `assets/shaders/ray_hit_lighting.glsl`, `assets/shaders/fibonacci.glsl`

**On hold:** off by default (`GlobalIlluminationSettings::enabled` and the ray tracing master switch), tests parked (`runParkedTests`); resumes in phase 17.

Bounce light from probes traced with hardware ray queries (`ray-tracing.md`). Method, formulas and the papers' numbers: `docs/research/ILLUMINATION.md`. Placement, relocation and states: `ddgi-cascades.md`. Fast lights, ray budget, adaptive memory, scene events, sharing: `ddgi-memory.md`.

## When probes run

- `VulkanRenderer::tracesProbes`: `VulkanDdgi` exists (GPU has ray queries, both atlas formats storage- and sample-capable) and `rayTracing` and `enabled` are on and the frame has `FrameLighting::probeCascades` (a game's `ProbeCascades`; every game places one).
- Otherwise nothing is traced or read: the flat ambient, dimmed by the sky visibility grid, stands in (the grid runs only on frames without probes).
- Frames without probes draw bit for bit as without the feature: sampling is behind a specialization constant (below).

## Settings

`GlobalIlluminationSettings` (`render/gpu/ddgi.h`, `RenderSettings::globalIllumination`, F5 Global illumination). Every field and its reason: the header. Groups:

| Group | Fields | Where described |
|---|---|---|
| Switch | `enabled` | here |
| Cascades | `probeCascades`, `probeSpacing`, `densestProbes*`, `cascadeProbes*`, `seedProbes` | `ddgi-cascades.md` |
| Rays | `raysPerProbe` (256), `probeUpdatesPerSecond` (60) | here, `ddgi-memory.md` |
| Memory | `irradianceTimeConstant` (2 s, by eye against flicker in a dark room), `distanceTimeConstant` (1 s) | here |
| Adaptive memory, events | `adaptiveMemory`, `changeWindowSeconds`, `changeThreshold`, `sceneEvents`, `event*` | `ddgi-memory.md` |
| Sharing | `probeSharing`, `fastProbeSharing` | `ddgi-memory.md` |
| Fast lights | `fastIrradianceTimeConstant`, `fastRaysPerProbe`, `fastLights`, `fastBlendMaxFrameSeconds` | `ddgi-memory.md` |
| Sampling | `selfShadowBias` (0.3), `bounceEnergy` (0.85), `lightsPerHit` (4, at most `DDGI_MAX_HIT_LIGHTS` 8) | here |
| Relocation, states | `probeRelocation`, `probeStates`, `wakeCells` | `ddgi-cascades.md` |
| Debug | `showProbes`, `shownProbeCascade`, `probeBallRadius` | here |

**Presets** (`quality_preset`): rays a probe (still and fast) Low 32, Medium 64, High and Ultra 256; cascade layouts and sharing in the other two files.

## Atlases

Every cascade's probes in one set of atlases, shared by the frames in flight (storage, sampled and transfer images).

| Image | Format | Texels a probe (inner + 1 border) |
|---|---|---|
| Still irradiance | RGBA16F (alpha: adaptive memory's history) | 10 x 10 (`DDGI_IRRADIANCE_TEXELS` 8) |
| Fast irradiance | RGBA16F | 10 x 10 |
| Distances (mean, mean square) | RG16F (`shaderStorageImageExtendedFormats`) | 18 x 18 (`DDGI_DISTANCE_TEXELS` 16) |
| `changeStatistics` | RGBA16F, written only by the irradiance update | 10 x 10 |
| `sharedIrradiance`, `sharedFastIrradiance` | RGBA16F, while sharing is on | 10 x 10 |

- **Octahedral mapping** (`octahedralEncode`, `octahedralDecode`): direction / (|x|+|y|+|z|), lower half folded over the diamond's edges; the sphere fills -1..1. `probeTexelDirection(texel, side)` is an inner texel centre's direction.
- **Borders** (`probeBorderSource`): edge texel copies its inner neighbour mirrored about the edge's middle, corner the opposite inner corner; `probeBorderTexel` numbers them for the update's copy (4 side + 4, within a group).
- **Layout:** slots x first, then y, then z (`probeIndex`; scrolled, `probeSlot`), cascades one after another from each `ProbeCascade::firstProbe`. Probe i's square at column i mod columns, row i / columns (`probeTileOrigin`, `probeAtlasSize`); columns the fewest whose square holds every probe (`probeAtlasGrid`), since cascades differ in width. `probeAtlasUv(grid, probe, direction, side)` is where to sample. A probe's cascade: `probeCascadeHolding` (`probe_cascade_holding.glsl`).
- **Limits:** `validateProbeVolume` throws on an empty axis, non-positive spacing or more than `maxProbesPerVolume` (65,535: one workgroup per probe); atlas side at most `DDGI_MAX_ATLAS_SIDE`. About 9 KB a probe.

## Rays

- `sphericalFibonacci(index, count)`: z in equal steps, golden angle round.
- **Ray cycle:** frames take turns through one spiral of count x `DDGI_RAY_CYCLE` (16) points. `probeRayFrame(frameNumber)` gives phase (frame mod 16) and the cycle's rotation (`randomProbeRotation(frame / 16)`, Shoemake from `mixBits`); `probeRayDirection(ray, count, phase, rows[, turn])` = point ray x 16 + phase, turned (rows from `probeRotationRows`). A cycle covers the sphere as evenly as 16 times the rays.
- Probes near fast lights use the same cycle (shorter cycles repeat directions and flicker more).
- `probeRayStride` (the larger of the still and fast counts) is each probe's room in the ray buffers; counts `DDGI_MIN_RAYS` 16 to `DDGI_MAX_RAYS` 1024.

## Update maths

Mirrored in `ddgi.glsl`; tested in `ddgi_test` and `ddgi_gpu_test` (`GPU_TEST_PROBE_*`).

| Function | Rule |
|---|---|
| `irradianceRayWeight` | Cosine, none from behind |
| `distanceRayWeight` | Cosine to the power `DDGI_DISTANCE_SHARPNESS` (50) |
| `distanceForUpdate` | Back-face hit (stored negative) keeps `DDGI_BACK_FACE_DISTANCE_SHARE` (0.2) of its length; all capped at `probeMaxDistance(spacing)` (1.5 cell diagonals), which a miss becomes |
| `hysteresisForTimeConstant(dt, tau)` | exp(-dt / tau): 1/e of a step left after tau at any frame rate |
| `blendProbeValue` | Irradiance blended linearly as it is (not the 2021 fifth roots, which settled sparse light dark and passed noise on) |

- CPU furnace test: a uniform sky stores itself exactly.
- `GpuProbeVolume` (exactly 128 bytes, the minimum push constant size; `ddgi_volume.glsl`'s `volume`): atlas grid and ray stride, cascade count and total probes, the frame's rotation rows (w: still rays, fast rays, change window's share kept), sky (w: change threshold), hysteresis (irradiance, distance, ball radius, ray phase), fast hysteresis. Whole numbers stored as floats are read with `volumeWholeNumber`; `volumeRayCount(nearFast)`, `volumeRayDirection`. Packed by `packProbeVolume(counts, cascades, ProbeFrame)`.

## Sampling

`render/gpu/ddgi_sampling`, mirrored by `ddgi_sampling.glsl`, tested in `ddgi_sampling_test`, `ddgi_sampling_gpu_test`. Follows the 2021 reference code.

1. **Cell:** `probeSelfShadowBias(n, v, spacing, bias)` = (0.2 n + 0.8 v) x 0.75 x smallest spacing x setting. `probeCell` finds the cell round the biased point (corner clamped, alpha per axis), `probeCellCorner` its 8 corners (bit 0 x, 1 y, 2 z), `probeTrilinearWeight` (each factor at least 0.001).
2. **Corner weight:** `probeCornerWeight` = `crushProbeWeight(probeBackFaceWeight x probeVisibility)` x trilinear.
   - Back face: ((cos + 1) / 2)^2 + 0.2, from the unbiased offset.
   - Visibility: 1 up to the stored mean distance, beyond it Chebyshev's bound cubed, at least 0.05 (`docs/research/ILLUMINATION.md`).
   - Crush: below 0.2, w^3 / 0.04.
3. **Blend:** `addProbeSample`, `finishProbeBlend`: square roots of the irradiances, weighted, averaged, squared. `ProbeLight {diffuse, specular}` (specular: same corners read in the mirror direction, a fully rough stand-in); `ProbeLights {still, fast}`, each blended alone and summed by the caller.

- **Only traced probes count** (`ProbeLookup::state`; GPU: set 1 binding 16): off and sleeping corners skipped, each corner where relocation moved it; no traced corner gives the flat ambient (`ProbeLookup::ambient`) and no fast light.
- **Cascades** blend densest first (`sampleProbeCascades`, `sampleTracedProbes`): each adds light x its weight x what the denser left; the flat ambient takes the rest of the still light, fast light fades with the cascades, so the edge has no seam.
- `sampleProbes(...)` is the whole loop on the CPU, for tests.

**Lights at probe rays' hits:**

- `pointLightAtHit`, `spotLightAtHit`: matte irradiance before the shadow ray (`distanceFalloff`, `spotCone`, cosine).
- `keepBrightestLight`: brightest first by `hitLightStrength` (Rec. 709 luminance); zero never joins; ties keep the earlier.
- `probeHitLights(lights, cameraOrigin, volume, keepFastApart)`: points then spots whose range sphere reaches the coarsest cascade's box grown by `probeMaxDistance`, as `GpuProbeHitLight {light, fast}` (from `GpuLights::fastPoints`/`fastSpots`), counting the fast ones. Set 1 binding 14 (`ray_hit_lighting.glsl`).

**Frame block:** `packProbeSampling` fills `GpuFrameData`'s `probeShading` (bias, bounce energy), `probeAtlas` (atlas grid, cascades), `probeLights` (points, spots, lights a hit keeps); zero while probes are not sampled.

## Shaders reading the probes

| Set 1 binding | Holds | Declared in |
|---|---|---|
| 12, 13, 15 | Still irradiance, distances, fast irradiance (sampled; `noProbesTexture` without) | `scene_probes.glsl` |
| 14 | Hit lights | `ray_hit_lighting.glsl` |
| 16 | Probe states (`noProbeStates` without) | `scene_probe_states.glsl` |
| 20 | Cascade records (`noProbeCascades` without) | `scene_probe_cascades.glsl` |

- `scene_probes.glsl`: `sampleProbes(cascade, ...)` (one cascade's corner loop, specular only when asked) and `probeAmbient(point, ..., fromCascade, withSpecular)` (cascades blended). Everything camera-relative.
- `triangle.frag` samples only with `DDGI_SAMPLE_PROBES_CONSTANT` on: visibility and back-face weights along the geometric normal, light along the shaded normal and its mirror, shaded by `shadeProbeAmbient` instead of `shadeAmbient`. Main pipelines come in probe and no-probe variants (`mainPipelineIndex(afterPrepass, sampleProbes, ...)`); transparent and capture pipelines too. A constant rather than a branch, since a branch changed the rest of the shader's code and moved pixels.

## VulkanDdgi per frame

`render/vulkan/vulkan_ddgi`. Around it `drawScene` makes the cascades (`probeCascadesAround`, round `cameraOrigin` or `ProbeCascades::centre`) and the hit lights.

**`prepare`** (before the scene set is written):

- New cascade size or spacing: `waitIdle`, new atlases cleared to zero, new per-probe buffers; first update takes rays outright.
- Moved cascades: brought-in planes pending relocation.
- Reads this frame in flight's relocation records, works out states, restarts, the traced list and seeds (`ddgi-cascades.md`) into per-frame `UploadArray`s (`stateUploads`, `cascadeUploads`, `tracedUploads`), reserves rays (`reserveRays`).

**`update`:** `scheduleProbeFrame` (its `ProbeSchedule` kept), `advanceProbeRestarts` into `restartUploads` (read where written: the fence ended last reads), then the passes, leaving atlases `SurfaceRead`.

| Pass | Shader | Work |
|---|---|---|
| Relocate | `ddgi_relocate.comp` | When asked or on a geometry event; records to read-back memory |
| Seed | `ddgi_seed.comp` | When probes start from coarser light |
| Trace | `ddgi_trace.comp` | 64 invocations a group (`DDGI_TRACE_GROUP_SIZE`), one per ray of the stride per listed probe |
| Mirror bounce | `ddgi_mirror.comp` | With mirror pairs: fast rays' front-face hits again, adding mirrored light (`mirrored-lights.md`) |
| Update | `ddgi_update_irradiance.comp`, `ddgi_update_distance.comp` | One group per listed probe, one invocation per inner texel |
| Share | `ddgi_share.comp` | While either sharing weight is above 0 |

**Trace** (scene sets 0-2 plus set 3: still rays as `vec4` light + distance, fast rays as half floats in a `uvec2` (`packFastRayLight`), a near-fast-light flag per probe, turns, traced list):

- `anyFastLightNear` picks the probe's ray count; ray 0 writes the flag; invocations past the count stop.
- Rays against `RAY_MASK_SOLID`, reach `DDGI_RAY_REACH`:
  - **miss:** the sky (`FrameLighting::background`) at full reach;
  - **back face:** no light, minus its distance;
  - **front face:** emissive plus `matteColour` (none for metals; probes average gloss away) x irradiance = directional lights by shadow ray (from `DDGI_SHADOW_RAY_OFFSET`, 1 cm, out) + the brightest `lightsPerHit` point and spot lights by shadow ray (fast ones into the fast sum) + last frame's probes (`probeAmbient`, diffuse) x `bounceEnergy`, still into still and fast into fast: one more bounce a frame.
- Shadow rays, not the shadow atlas, which covers only the view.

**Update** (`ddgi_update.glsl`; bindings 0-2 ray buffers, 3 turns, 4 traced list, 5 restarts, 6 states, 7 cascades, 8 seeds, 9+ atlases):

- Ray directions and results stream through shared memory `DDGI_UPDATE_RAY_CHUNK` (256) at a time, summed in ray order: same sums for any count, and 1024 at once would exceed shared memory.
- Summed weight below `DDGI_MIN_TEXEL_WEIGHT` keeps the old value; else blend (still and fast into their own atlases, distances as mean and mean square), `imageStore` in place, then the border copy.
- An unlisted probe has no group and keeps its texels.

**Barriers:** ray buffers wait for last frame's update reads before the trace writes; atlases go `SurfaceRead` → `ComputeStorage` (after this frame's trace and last frame's main pass and balls) and back.

## Golden images

- A view with probes draws `goldenProbeWarmUpFrames` (960: four irradiance time constants, `engine/golden.h`) frames first, each a tick long (`VulkanRenderer::capture`'s `probeWarmUpFrames`, `drawFramesBeforeCapture`); events go to the first warm-up frame only.
- `Application::captureGoldenView(view, probeWarmUpFrames)` takes other counts: captures N and N + k frames apart measure flicker over k frames.

## Debug view

`showProbes` (F5): `drawProbes` after the main pass (`GpuPass::ProbeBalls`, BALLS).

- `ddgi_probes.vert`: a square per probe (6 corners, no vertex buffer) through the ball's centre facing the camera, half side r d / sqrt(d^2 - r^2), so balls at the view's edge are not cut.
- `ddgi_probes.frag`: ray-sphere hit (misses discarded), true depth written, stored irradiance (still + fast) along the normal: a white matte ball lit by the probe.
- One cascade at a time (`shownProbeCascade`, 0 densest), balls scaled with its spacing, at relocated positions.
- Untraced probes show their state: off red, sleeping dark blue, awake a yellow rim round its light.
