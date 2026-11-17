# Reflections

Covers: `src/render/gpu/screen_reflections`, `src/render/gpu/screen_reflections_shared.h`, `src/render/vulkan/vulkan_reflections`, `src/render/vulkan/vulkan_colour_pyramid`, `assets/shaders/reflections_pass.glsl`, `assets/shaders/reflections.comp`, `assets/shaders/reflections_rays.comp`, `assets/shaders/screen_reflections.glsl`, `assets/shaders/depth_pyramid.comp`, `assets/shaders/colour_pyramid.comp`, `assets/shaders/triangle_surfaces.frag`, `assets/shaders/triangle_surfaces_rays.frag`

The reflection pass: how each glossy pixel's stand-in is replaced, the depth and colour pyramids, and screen-space reflections. The probe's search: `reflection-probe.md`. Flat mirrors: `planar-reflections.md`. Light reaching surfaces off mirrors: `mirrored-lights.md`. Mirror rules: `.claude/rules/shader-mirror.md`.

## Sources, in order

The main pass adds a **stand-in** for each pixel's reflection: the light probes' (or the flat ambient's) light in the mirror direction times the specular albedo. The reflection pass swaps it, as far as each source is trusted:

| Step | Source | Where |
|---|---|---|
| 1 | Reflection probe's parallax search, or with `rayTraced` the traced mirror ray | `reflectFromProbe`, `reflectFromRays` |
| 1b | On a flat mirror: the mirror's picture, replacing step 1 | `isOnPlanarMirror` |
| 2 | Screen-space reflection of last frame's picture, on top (not on a flat mirror) | `reflectFromScreen` |

- `reflectedScene`: scene + confidence × (specular albedo × light − stand-in), never below 0.
- Step 1 goes in first; `blendedStandIn` (its blend with the stand-in) is what step 2 then replaces.
- Where neither is trusted at all the pixel is not written: exactly the main pass's picture.
- Glossy means: not sky, roughness below `maxRoughness`, mirror ray leaving the surface (normal maps can tilt it in).
- Reflections in reflections build up a bounce a frame: last frame's picture holds last frame's reflections.

## When it runs

`VulkanRenderer::drawScene` (all in `render/vulkan/vulkan_renderer.cpp`):

- `screenReflections` = anti-aliasing on and `ReflectionSettings::enabled`: reflections read last frame's smoothed picture and the main pass's surfaces.
- Ray-traced (`reflections_rays.comp`) when `tracesRays` (the master switch, `ray-tracing.md`), `rayTraced`, and the GPU has ray queries (`VulkanReflections::tracesRays`).
- Screen-space part when `screenSpace` and `previousPictureUsable` (last frame was smoothed and nothing was remade since) and the previous camera is known.
- The pass runs when any of: screen-space part, a finished probe capture, ray-traced reflections, or a flat mirror this frame.
- Timed as SSR (`GpuPass::Reflections`): after CUBE and PLANAR, before the fog's picture pass, the glass and anti-aliasing; in place on the scene image (which has storage use).

## The main pass's surfaces

With reflections on, the main pass writes three more targets at the render size (outputs 1-3 of `triangle_surfaces.frag`, or `triangle_surfaces_rays.frag` with glass rays; `scene_surface.glsl` with `REFLECTION_SURFACES`), so pipelines without them hold none of the code:

| Target | Format | Holds |
|---|---|---|
| Normal | `R16G16_SFLOAT` | shaded normal, octahedral (`octahedralEncode`) |
| Properties | `R8G8B8A8_UNORM` | specular albedo; roughness in alpha (1 without a normal) |
| Stand-in | `B10G11R11_UFLOAT`, RGBA16F where that cannot be drawn into | what the pixel added for its reflection |

- Not cleared: the pass reads only pixels with a surface (`VulkanReflections::beginSurfaces` discards old contents).
- `mainPipelines` is indexed by `mainPipelineIndex` (after the prepass, probes, surfaces, cut-outs without the prepass, glass rays); `PipelineDescription::extraColourFormats` names the extra targets.
- Sky and stars pipelines draw into the scene image alone, so with surfaces they go in a second rendering block after the main one (colour and depth loaded; depth stored for it).

## The pyramids

| Pyramid | Shader | Format | Level 0 | Each level up |
|---|---|---|---|---|
| Depth (`VulkanReflections::buildDepthPyramid`) | `depth_pyramid.comp` | R32F, full chain, at most `SSR_MAX_PYRAMID_LEVELS` (16) | this frame's depth | nearest (reversed-Z: largest) of the 2x2 below |
| Colour (`VulkanColourPyramid`) | `colour_pyramid.comp` | RGBA16F, same levels | last frame's finished picture (`finishedScene`: TAA's history, FSR's or DLSS's output) resampled to the render size | average of the 2x2 below |

- An odd level's far edge also takes the leftover texel (`pyramidLastFine`), so every texel has a cell above it (`pyramidCell` clamps).
- One dispatch a level, a barrier between levels; push constants `GpuPyramidPass`.
- The colour pyramid is built early (`prepareColourPyramid`, timed PYRAMID) when ambient occlusion gathers light from it; `reflect` then reuses it, and `forgetColourPyramid` marks it stale each frame.
- `VulkanColourPyramid` is shared: refraction builds one from the picture before the glass, and a flat mirror's picture has its own.

## The screen-space march

`reflectFromScreen` (`reflections_pass.glsl`), one invocation per render pixel, `SSR_GROUP_SIDE` (8) square:

1. **Start** `SSR_START_OFFSET_PIXELS` (2) pixel footprints off the surface along its normal (`pixelFootprint`: a pixel's world size per metre, from the view-projection's second row), so the surface it leaves does not hide it.
2. **Project** (`screenRay`): pixel x, y and reversed-Z depth are all linear in t (depth is affine in 1/w, as x/w and y/w), so the ray is cut like a box by the screen's edges and the depth range, after its far end is cut to `SSR_NEAREST_W` in front of the camera.
3. **March** (`hizStep`, McGuire and Mara 2014, Uludag 2014): in front of a cell's nearest depth, skip to where the ray reaches it (a level finer) or out of the cell, `SSR_CELL_CROSSING` texels into the next (a level coarser); at or behind it, a level finer without moving; below level 0, a hit. A hit more than `thickness` behind the surface (view distance, `withinThickness`) is something the ray passes behind: `passTexel` and go on. At most `maxSteps`. CPU mirror of the loop: `traceScreenRay` on a `DepthPyramid`.
4. **Reproject** the hit to last frame's picture: by its motion if it moved (velocity image), else through the two cameras (`reprojectPixel`, as TAA), from its centre without this frame's jitter. Nothing where it was off that picture.
5. **Read** the colour pyramid at `reflectionConeLevel`: the reflected cone's half angle has tangent `SSR_CONE_SPREAD` × alpha (GGX's lobe at half its peak, the mirror turning twice as far); the level is log2 of its width after the pixels travelled.
6. **Trust** (`reflectionConfidence`), the product of five fades:

| Fade | Full | None |
|---|---|---|
| Roughness | up to `sharpRoughness` | from `maxRoughness` |
| Screen edge | inside `edgeFade` of the half-width | at the edge |
| Reach | first 80% (`SSR_DISTANCE_FADE_START`) | at `reach` |
| Front face (hit normal · ray) | from −`SSR_FRONT_FACE_COSINE` (−0.2) | at 0 (grazing) and behind |
| Towards the camera (ray · way back to it) | up to `SSR_TOWARDS_CAMERA_START` (0.3) | from `SSR_TOWARDS_CAMERA_END` (0.7) |

The last fade exists because such rays can only really hit what lies between the surface and the camera, yet they pass just behind near surfaces on screen and count as hits within the thickness; a mirror seen head-on is left to the probe, the planar picture or rays.

## Ray-traced reflections

`reflectFromRays` (`REFLECTION_RAYS`), in place of the probe's search:

- The mirror ray leaves lifted along the normal (`PROBE_SHADOW_RAY_OFFSET` + `PROBE_SHADOW_RAY_OFFSET_PER_METRE` per metre), mask 0xFF (glowing stand-ins for lights included), as far as `probeFar`.
- **Miss:** the background plus the atmosphere's light that way (`skyLightTowards`).
- **Back face:** nothing.
- **Front face:** the probe's picture of the hit (its direction from the probe, at the roughness's level) trusted by `parallaxConfidence` of the probe's distance to the hit against the stored one; elsewhere the hit lit as light probe rays light theirs (`ray_hit_lighting.glsl`: `lightAtRayHit`, matte, plus `mirroredLightAtRayHit`), blended by that trust.
- Glass on the way tints it (`glassTransmittance`).
- Trust is the roughness fade alone; screen-space reflections still go on top.
- Sets: textures, the frame's scene and the ray tracing set first (`ComputePipelineDescription::sharedSets`, `ReflectionRaySets`), the pass's own as set 3; the ray tracing scene is built for it.

## The pass record

`GpuReflectionPass` (`packReflectionPass`, std430, 352 bytes, a buffer per frame in flight): view-projection and inverse, the reprojection, size, the settings, the pyramid's top level, pixel footprint, jitter, the probe (levels, origin from the camera, size, distance levels, far plane), the background, and the flat mirror's plane and levels. What the frame offers besides the settings comes in as `ReflectionPassFrame`.

Set 0 (set 3 with rays): the record, the depth pyramid (exact texels, every level), the colour pyramid (trilinear), the three surfaces, the velocity, the probe's prefiltered light and its distances (exact and blended), the planar picture, the scene image. The probe's images are made and bound even with the probe off.

## Settings

`ReflectionSettings` (`RenderSettings::reflections`, F5 Reflections, keys `engine.render.reflections.<field>`; `validateReflections`). Header for each field; the ones that shape behaviour:

| Setting | Default | Note |
|---|---|---|
| `enabled` | on | Low: off |
| `screenSpace`, `probe` | on | each source off alone, for comparing |
| `sharpRoughness`, `maxRoughness` | 0.3, 0.6 | the roughness fade, shared by every source |
| `thickness` | 0.3 m | depth holds only front faces; without it a ray behind a thin pole hits it |
| `reach` | 1000 m | far hits on screen are as good as near ones |
| `maxSteps` | 64 | Medium and Low: 32 |
| `rayTraced` | on | Ultra only; needs ray queries and the master switch |
| `showConfidence` | off | debug view: white screen-space, blue probe, black stand-in |

Probe and planar fields: their own files.

## Where to judge

The light lab: the polished slab and mirror ball (`labPolishedSlab`, `labMirrorBallCentre`), the sunlit room's mirror facing the doorway (`labMirror`) and the brushed steel panel beside it (`labSteelPanel`, roughness 0.2), all in `games/lightlab/light_lab_level.h`.

## Tests

- `screen_reflections_test`: pyramid sizes, cells and the odd edge, `screenRay` cuts, `hizStep`, `traceScreenRay`, the confidence's fades, cone level, `reflectedScene`, validation, the packed record.
- `screen_reflections_gpu_test`: the library against its mirror.
