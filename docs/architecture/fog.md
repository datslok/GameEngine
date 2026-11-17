# Volumetric fog

Covers: `src/scene/fog`, `src/render/gpu/volumetric_fog`, `src/render/vulkan/vulkan_fog`, `assets/shaders/volumetric_fog.glsl`, `assets/shaders/scene_fog.glsl`, `assets/shaders/scene_fog_shadows.glsl`, `assets/shaders/fog_light.comp`, `assets/shaders/fog_integrate.comp`, `assets/shaders/fog_apply.comp`

Light the air scatters towards the eye: beams and lamps light the mist round them, distant things fade. Single scattering on a froxel grid, then a closed form past it. Shader maths rules: `.claude/rules/shader-mirror.md`; numbers in `volumetric_fog_shared.h`.

## The physics

Along a view ray to a surface at distance d:

  L_eye = T(d) L_surface + ∫₀ᵈ T(s) σ(s) albedo S(s) ds,  T(s) = exp(−∫ σ)

S: light arriving at s from each light, times the phase function and its shadow there. In the engine's units (a light's value is irradiance E, a pixel π × radiance): a light adds `fogInScattering` = π p(g, cos) E per unit of optical depth; the light from all around is `fogAmbient`, the mean of the irradiance from above and below.

## Inputs

- **`Fog` component** (`scene/fog.h`; the first in the world goes into `FrameLighting::fog`): `density` (extinction per metre at `baseHeight`; 3 / density ≈ visibility), `scaleHeight` (falls by e per scale height up, grows below; `fogDensity`, exponent capped at `FOG_MAX_EXPONENT`), `albedo` per colour, `anisotropy` (Henyey-Greenstein g, `henyeyGreensteinPhase` shared with the sky), `maxDistance` (no fog beyond along a ray, so the sky shows).
- **`FogSettings`** (`RenderSettings::fog`, F5 Fog, `validateFog`; meanings in `render/gpu/volumetric_fog.h`): `enabled` (off on Low), `range` (100 m, never past the far plane), `gridWidth`/`gridHeight`/`slices` (160 × 90 × 64; presets lower; changing them remakes the grid after a GPU wait), `historyWeight` (0.9), `probeLight`, `farShadowSteps` (16).

## The grid

Froxels shaped like the view through the unjittered camera (`motionViewProjection`), camera-relative:

- Place = (across 0..1, down 0..1 from the top, slice 0..1): `fogVolumeCoordinate` (clip w is view depth) and back by `fogVolumePoint`.
- Slices even in log(depth + `FOG_SLICE_START`) (`fogSliceDepth`, `fogDepthSlice`): about 7 cm deep at the camera, 7 m at 100 m for 64 slices, as a froxel's width grows with distance.
- Froxel (x, y, k)'s middle is ((x, y, k) + 0.5) / size.

**`GpuFog`** (272 bytes, std430, `SceneBuffers::fog`, set 1 binding 27, `packFog`): the grid's camera, its inverse, last frame's taking this frame's points (`previousRelativeViewProjection`), grid size and range, the medium, albedo with `maxDistance` in w, options (x on; y history weight, 0 with no usable history; z read probes), the jitter. All zeros without fog. Binding 28: the integrated grid (`VulkanFog::sampledVolume`; a 1 × 1 × 1 clear stand-in without fog).

## The passes

`VulkanFog`: three compute pipelines on scene sets 0 and 1 plus their own set 2; RGBA16F volumes.

| Pass | Shader | Group | Timed | Where |
|---|---|---|---|---|
| Lighting | `fog_light.comp` | 4 × 4 × 4 froxels | FOG (`GpuPass::Fog`) | after shadows and clusters, before the depth prepass |
| Integration | `fog_integrate.comp` | 8 × 8 columns | FOG | right after |
| Picture | `fog_apply.comp` | 8 × 8 pixels | FOGMIX (`GpuPass::FogApply`) | after the reflections, before glass |

The shadow atlas goes to `DepthComputeRead` for the lighting pass and back.

### Lighting

**Sample point:** the froxel's middle moved by `fogJitter` (Halton 2, 3, 5 over `FOG_JITTER_FRAMES`), turned per froxel (`fogFroxelJitter`: Cranley-Patterson rotation by `fogFroxelTurn`, a PCG3D hash of the froxel shared with `core/hash.h`), clamped into the grid (`fogFroxelCoordinate`). Neighbours sample different places at once, so what the history leaves is fine noise, not one moving pattern.

**Its light:**

- Directional lights: `sunShadowInAir` (`scene_fog_shadows.glsl`): the finest cascade holding the point (biased towards the light), one hardware-filtered `shadowTap`, with the far shadow map's distant blockers added; lit past every cascade.
- Point and spot lights from the froxel's cluster lists (`clusterOfPixel`; all lights with clustered shading off): `distanceFalloff`, `spotCone`, one comparison against the light's tile. An image light (a lamp seen in a flat mirror) lights only air in front of its window, shadowed by its source's map at the crossing.
- From all around: with probes in the frame (`packFog`'s `probesRead`, `options.z`), `probeAmbientInAir` (`scene_probes.glsl`: cascades densest first, 8 corners weighted by `probeAirWeight`, read up and down); else the flat ambient times the sky visibility grid's air share (`skyVisibilityInAirAt`), so closed rooms are not lit as outdoors.
- Times the albedo; alpha holds the extinction at the point's height.

**History:** last frame's grid read where the froxel's middle was (`fogHistoryCoordinate`, filtered; with a still camera exactly its own texel, so no neighbour mixing that would make sunbeam edges shimmer), keeping `fogHistoryWeight`'s share (0 outside last frame's grid). Kept only when last frame drew fog with the same grid and range (`VulkanFog::prepare`). Two lighting grids take turns.

### Integration

Each column front to back, slice length = depth step × the column's stretch (1 / its cosine to the view axis); `fogIntegrateSlice`: light += scattered × T × (1 − e^−τ) (`oneMinusExp`, shared with the sky), T ×= e^−τ. Exact for constant fog in a slice (energy-conserving). Texel k holds the fog to the far side of slice k. Mirror: `integrateFogColumn`.

### The picture

`fog_apply.comp` turns the picture before the glass into the fogged one in place (`fogComposite`: colour × T + light), so refracting glass and the reactive mask read it fogged. Each pixel's point from depth (the sky anywhere along its ray). Push constants `GpuFogPicturePass` (80 bytes, `packFogPicturePass`: the jittered inverse, render size, far shadow steps, frame).

## Reading the fog along a view

`fogAlongView` (`scene_fog.glsl`; the picture pass uses `fogAlongViewShadowed`):

- **Near the surface:** never the slice the point is in, whose jittered sample may lie behind the surface (sunlit air past a wall streaked it). The grid is read up to one slice before the point (`fogSliceRead`, half a texel back, fading to none inside the first half slice); over the last stretch a medium (`FogMedium`, `fogSliceMedium`) slides from two slices before to one before as the point crosses its slice (`mixFogMedium`, `fogThroughMedium`, `fogSurfaceRead`). Continuous across slice boundaries, exact where slices are alike.
- **Past the grid** (range / cosine to the view axis, up to `maxDistance`): `fogBeyondGrid`, the height fog's closed form (`fogOpticalDepth`, series near level, capped looking down), lit by the directional lights and the flat ambient (`fogBeyondSource`; the ambient at the open field's air share `SKY_OPEN_FIELD_OPEN` while the sky visibility grid or probes light the nearer air, so the two meet), joined by `fogFollow`.

### The far fog in shadow

Picture pass only (`fogBeyondSourceShadowed`): each shadowed directional light's share is dimmed by the share of that stretch's light it reaches.

- **Samples of equal light** (`fogFarSampleDistance`): the stretch scatters 1 − e^−total in all, so share u sits at optical depth −ln(1 − u(1 − e^−total)), turned into metres by the closed form's inverse. `farShadowSteps` samples at (k + turn) / steps; even spacing in metres would over-count the thin far fog.
- Each sample: `sunShadowInAir`, as for the grid; the far shadow map (`ShadowSettings::farShadowDistance`) reaches kilometres.
- `turn` = `interleavedGradientNoise` of pixel and `taaFrame`; the anti-aliasing after averages it.
- Glass and the AO gather still see the far fog unshadowed.

## Glass

Drawn over a picture already fogged to the surface behind, so `scene_surface.glsl` applies `fogOverGlass`: the glass's own light dimmed by the fog in front plus the fog's light for the share it does not let through, per colour. Glass and the surface behind are then fogged in turn.

## Fog taken back off

`fogRemoved`: (seen − fog light) / T, never below black, T floored at `FOG_REMOVAL_MIN_TRANSMITTANCE`: for light gathered from the picture (ambient occlusion's bounce light).

## Barriers

- This frame's lighting grid: from last frame's reads to storage, discarded; last frame's readable by compute (made readable unwritten the first time, weight 0).
- Lighting grid to compute read for integration; integrated grid from last frame's reads (picture pass, glass) to storage, discarded, then `SurfaceRead`.
- Cluster list barriers include the compute stage on both sides.
- Picture pass: depth to `DepthComputeRead`, scene image to `ComputeStorage`.

## Not done

- Probes in empty air stay asleep, so air high above the ground reads coarser cascades.
- Lights without shadow maps light the fog through walls.
- No sun shadow past the last cascade (the far map).
- Refracting glass reads the fogged picture and adds the front fog for its covered share: that part counted twice.
- The fog does not dim light on its way to a froxel.
- The reflection probe's capture and probe rays see no fog.

## Tests

`volumetric_fog_test` (settings, medium, closed form against fine sums, slices and grid coordinates both ways, jitter and per-froxel turns, history coordinates, phase furnace, slab and column, slice read and surface medium, past the grid, far shadow samples, glass, packing), `quality_preset_test`, `gpu_timing_test`, `light_lab_test` (the mist); `volumetric_fog_gpu_test`, `ddgi_sampling_gpu_test` (`probeAirWeight`).
