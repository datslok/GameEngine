# Reflection probe

Covers: `src/render/gpu/reflection_probe`, `src/render/gpu/reflection_probe_shared.h`, `src/render/vulkan/vulkan_reflection_probe`, `assets/shaders/reflection_probe.glsl`, `assets/shaders/probe_capture.frag`, `assets/shaders/probe_capture_rays.frag`, `assets/shaders/probe_distance.comp`, `assets/shaders/probe_distance_min.comp`, `assets/shaders/probe_prefilter.comp`

One 360-degree picture taken at the camera while reflections run, for what the screen cannot reflect. Placed automatically: no boxes per level. How the reflection pass uses it: `reflections.md`. Mirror rules: `.claude/rules/shader-mirror.md`.

## When

- Captured while reflections run (anti-aliasing on, `ReflectionSettings::enabled`) and `probe` is on; `probeSize` changes remake the cubes (`VulkanReflectionProbe::ensureSize`, after a device wait) and forget the capture.
- Read by the reflection pass once a capture is finished (`hasCapture`), and by glass (`fragmentReadableLight`).
- Timed as CUBE (`GpuPass::ReflectionProbe`): after the main pass, motion pass and depth measurement, before PLANAR and SSR.

## Spread over frames

`probeCaptureStep` → `ProbeCaptureStep` (first face, face count, starts or finishes a cycle, next face):

- `probeFacesPerFrame` (1, live, held to 1-6) faces a frame; 6 draws every face every frame.
- Every face of a cycle is drawn from where the cycle started (`VulkanRenderer::probeCycleOrigin`): the face's view is moved by the camera's offset from it, since the capture's vertex shader measures from the camera; the sky's rays start there too.
- A cycle's first face discards the old contents; later faces keep earlier ones (`beginCapture(startsCycle)`).
- Mips, distances and prefiltered light are made after the sixth face (`finishCapture`), so reflections read the last finished capture, up to six frames behind.
- Nothing captured yet, or a new size: all six at once.

## The cube

Vulkan's cube order and addressing (+X, −X, +Y, −Y, +Z, −Z; `cubeFaceAxes`, `cubeTexelOf`, `cubeTexelDirection`), so shaders read it as a `samplerCube`.

- A face's camera (`cubeFaceViewProjection`): rows right, up and minus forward, square 90-degree lens, reversed-Z from `PROBE_NEAR` (0.05 m) to `probeFar`.
- Cube faces are mirror images of an ordinary camera's picture, so the capture pipelines cull front faces (shared with planar mirrors).

| Image | Format | Holds |
|---|---|---|
| Capture | RGBA16F cube, full mips | the light each face sees; mips feed the prefilter |
| Depth | D32, array of 6 | each face's depth |
| Distances | R32F cube, full mip chain | distance from the probe to each texel's surface (`probeRadialDistance`), the far plane for sky; levels above 0 the nearest of the 2x2 below |
| Light | RGBA16F cube, `probeLevels` (down to `PROBE_SMALLEST_LEVEL_SIZE`, 4) | light prefiltered for roughness 0 (level 0) to 1 (last), linear (`probeLevelRoughness`, `probeRoughnessLevel`) |

## The capture

`VulkanRenderer::drawProbeCapture`:

1. **Culling:** each face's opaque draws against its own frustum (`probeFaceCullView`), listed after the velocity pass's in the frame's draw list; on the GPU as a view of the camera's culling (`gpu-driven.md`), on the CPU (`captureDraws`) with `passes.gpuCulling` off. Each face has its own level-of-detail view (`levelView`, `probeSize` pixels across a quarter turn) and card thinning error (`probeThinning`), so it draws coarser levels than the camera.
2. **Shading:** `triangle.vert` takes the face's view-projection from push constants (`GpuCaptureView`: view-projection, clip plane (`noClipPlane` here), eye; specialization constant `PROBE_CAPTURE_VERTEX_CONSTANT`). `probe_capture.frag` is the main pass's shading (`scene_surface.glsl` with `PROBE_CAPTURE`) over every light, since the clusters are the camera's, with no reflection surfaces or heatmap. Pipelines indexed by `capturePipelineIndex` (light probes, cut-outs, rays).
3. **Sky:** drawn over each face's background (`packCubeFaceRays`, `sky.frag`) without the sun's and moon's discs (`skyWithoutDiscs`) and without stars: those lights' highlights come from the lights themselves, shadowed, and a copy in the probe would add them again unshadowed. The camera's and mirrors' skies keep the discs.
4. **Sun beyond its cascades:** the cascades fit the camera's view, so the capture sees surfaces they do not cover. With ray tracing on, `probe_capture_rays.frag` (`PROBE_CAPTURE_RAYS`, also tinting shadowed lights through glass) traces the sun's shadow there (`sunBeyondCascades`, `reachesLight` from `ray_scene.glsl`; offsets `PROBE_SHADOW_RAY_OFFSET`, `PROBE_SHADOW_RAY_OFFSET_PER_METRE`), in a layout with the ray tracing set as set 2. Without rays it counts as lit, as beyond the shadow distance.
5. **Mirrored light:** the traced capture runs the mirrored light search at each texel (`mirrored-lights.md`).
6. **Finish** (`finishCapture`):
   - mips (`generateMipmaps`, shared with texture uploads);
   - distances (`probe_distance.comp`) and their pyramid (`probe_distance_min.comp`; CPU mirror `buildProbeDistanceCube`);
   - prefiltered levels (`probe_prefilter.comp`, `prefilterTexel`, push constants `GpuPrefilterPass`): each texel averages `PROBE_PREFILTER_SAMPLES` (64) GGX samples round its direction for its level's roughness (Karis's split sum: normal and view along the direction), each read from the capture level as wide as its share of the lobe (`prefilterSourceLevel`, filtered importance sampling), weighted by n·l. Samples from the BRDF table's `ggxSampleHalfway`, `radicalInverse`, `alongNormal` (`render/gpu/brdf`).

## The parallax search

A picture from one point is wrong seen from another. For a pixel at P (from the probe) and mirror direction R, the ray P + R t meets a surface where it first goes behind what the probe saw in its own point's direction: `parallaxMismatch` = |P + R t| − D(direction) going from negative to positive (distance impostors, Szirmay-Kalos et al. 2005).

`searchProbe` (CPU mirror of the shader's loop, reading distances through callbacks):

| Stage | What | Numbers |
|---|---|---|
| Lift | start off the surface by `probeStartLift`: probe texels at the point's distance, over the cosine between the normal and the way to the probe | `PROBE_START_LIFT_TEXELS` 2, cosine at least `PROBE_START_LIFT_SLANT` 0.25 |
| Leave | from `PROBE_MARCH_START`, doubling until the ray is in front of what the probe saw | 2 cm, at most `PROBE_LEAVE_STEPS` 8 |
| Walk | cell by cell through the distance pyramid | at most `PROBE_MARCH_ITERATIONS` 64 |
| Halve | the texel's stretch of the ray where it went behind, on blended distances | `PROBE_REFINE_STEPS` 8 (256 times narrower) |

**Reading the distances** three ways: a level's exact texel (nearest sampler, every level); level 0 blended between texels (a floor at a slant reads as continuous); level 0's four texels round a direction (`textureGather`, `probeDistanceSpread`).

**The walk:**

- A cell's edges are planes through the probe, so where the ray leaves one is one division per edge (`cubeCellOf`, `cubeCellExit`; step past by `PROBE_CELL_CROSSING_SHARE`, at least `PROBE_CELL_CROSSING`).
- Ray farther than the cell's nearest distance all across it (a line's distance from the probe peaks at an end): on to the next cell, a level up. Else a level down without moving.
- At level 0, the blended distance where it leaves the texel says whether it went behind.
- Far behind the texel's own surface yet in front where it leaves: it passed behind something thin the probe saw in front (a wall edge-on), and returns nothing.
- **Early stop:** once the probe's line of sight to the ray (`parallaxDirection`) is within a texel (`probeTexelAngle`) of the ray's own direction, the probe sees what the rest of the ray sees: read along the ray. Past the far plane: escaped the same way.

Why a walk rather than solving |P + R t| = D: the equation has other roots further on, through doorways and behind walls, and fixed steps skip walls seen edge-on.

**Confidence**, the larger of:

- `parallaxConfidence` (blended distances either side of the crossing agree: full within `PROBE_MATCH_SHARE` 3%, none past `PROBE_MISMATCH_SHARE` 15%) × `probeEdgeConfidence` (the four gathered texels spread less than `PROBE_EDGE_JUMP_START` 20% to `PROBE_EDGE_JUMP_END` 40%: one surface, not an edge);
- `probeViewConfidence`: the probe's direction to the found point within one texel of the ray's (full) to two (none). Far things seen from near the probe are trusted this way across silhouettes.

**Reading:** the prefiltered light in the found direction at `probeRoughnessLevel`, faded by the roughness fade of `reflections.md`.

**Where it gives up** (without ray tracing): the probe's picture along the mirror ray stands in for the untrusted share (`probeGuessedLight`), the result taking the full roughness fade as its confidence, rather than the flat stand-in, which drew bright outlines round reflected doorways. `blendedStandIn` is what screen-space reflections then replace.

## Settings

In `ReflectionSettings` (`reflections.md` for the rest):

| Setting | Default | Note |
|---|---|---|
| `probe` | on | off: screen-space reflections alone |
| `probeSize` | 256 | power of two, 16-1024; High 128, Medium and Low 64; each doubling four times the capture |
| `probeFar` | 300 m | beyond: the sky |
| `probeFacesPerFrame` | 1 | 6: every face every frame |

## Tests

- `reflection_probe_test`: cube addressing, face views and rays, the parallax march (a doorway the ray passes beside, through and across the edge of), the distance cube and its pyramid, `searchProbe` on a 256-texel doorway scene, levels, the prefilter, `probeGuessedLight`, `blendedStandIn`, the capture schedule.
- `reflection_probe_gpu_test`: the library's functions against their mirrors (planar mirrors' `isOnPlanarMirror` and `planarClipDistance` too).
