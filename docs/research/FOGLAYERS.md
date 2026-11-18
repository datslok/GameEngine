# Layers of fog

Several fog media at once: the valley's thin haze over kilometres, and a dense floor mist in the jungle that tops out under the canopy (phase 18). Later: local fog volumes (spheres, boxes) for mist in hollows, over water and round the structures. How the fog works today: `docs/architecture/fog.md`.

## How shipped engines do it

| Engine | What | Numbers |
|---|---|---|
| Unreal 4.16+, 5 | Exponential Height Fog's **Second Fog Data**: a second exponential layer with its own density, height falloff and height offset, added to the first | Epic's tutorial: density 0.3, falloff 1.8, offset 7500 (cm) to thicken fog in low pits; density 0 turns it off |
| Unreal 5.3+ | **Local Fog Volumes**: spheres of fog fading radially, each with radial density and a height fog inside (density, falloff, offset); voxelized into the volumetric fog's froxels when that is on, else composited by distance to their centre | height falloff 1.0 the lowest before artefacts at the horizon |
| Frostbite (Hillaire, SIGGRAPH 2015) | One participating-media framework: height fog, local volumes (boxes) and particles all voxelized into one froxel grid of scattering and extinction, then integrated; checked against Mitsuba | media add: the sums of extinction and of scattering × albedo per froxel |

What they share: media **add**. Extinction σ = Σ σᵢ; scattered light per unit length = Σ σᵢ albedoᵢ p(gᵢ) E. The froxel grid needs no other change.

## Physics for the engine

- **Exponential layer** (today's): σ(h) = d e^((b − h)/H), capped below.
- **Pooled** (new, `FogShape::Pooled`): σ(h) = d for h ≤ top, d e^(−(h − top)/edge) above. Mist pooled under a ceiling: cold air drains into the valley and stays under the canopy, the warmer air above holds it down (a temperature inversion). Heights are absolute, as the cold pool is, not following the ground.
- **Along a ray** (closed form, past the grid): a straight ray crosses h = top at most once, so a pool's optical depth = d × (length below top) + today's exponential form from the crossing on. Exact.
- **Total along a ray** = Σ layers. The far fog lit by unshadowed light: Σ Sᵢ ∫σᵢ T ds; taken as (Σ Sᵢ τᵢ / τ)(1 − e^−τ), exact when the layers share albedo and phase (both water droplets: they do here), else a small error far away.
- **Far shadow samples** (`fogFarSampleDistance`): invert τ(s) = target. With one exponential layer it has a closed form (kept, the fast path). A sum of layers has none: Newton on τ(s) with τ'(s) = σ(s) > 0, bracketed so a bad step falls back to bisection, about 4-6 steps.
- **Sight**: about 3 / σ. Jungle floor mist with 60-150 m of sight: σ ≈ 0.02-0.05 per metre (the valley haze is 0.0005).
- **The forest's heights** (`forest_trees.cpp`): canopy trees 38 m forking at 18 m, emergents 55 m at 36 m, midstorey 15 m at 10 m (before each placement's scale). A pool topping at about 12 m with a 4 m edge sits under the canopy's crowns and round the midstorey's.
- **Sun through the mist**: the fog does not dim light on its way to a froxel today (`fog.md`, Not done). Down through 10 m of σ = 0.03 the sun loses 26%; at a low sun much more. For a directional light it has a closed form per layer (the same optical depth, towards the light, to the top of the medium), one per froxel and layer.

## Cost

Grows with the froxels (fixed grid) × layers and the pixels × layers × far samples; nothing with the world's size. Unused layers are skipped by a count. Each layer: a few more exps per froxel and pixel, one more phase function per light per froxel, Newton steps per far sample only with two or more layers.

## Planned

- **B, layers** (`fog-layers`): up to `FOG_MAX_LAYERS` (4) `Fog` components count, each exponential or pooled; the forest's Mist gets a floor pool beside the haze.
- **C, local volumes** (later): spheres and boxes of fog culled into the clusters' froxels like lights, cost growing with volumes in view; then noise in the density near the camera (drifting patches), only inside the grid.

## Sources

- [Unreal: Exponential Height Fog](https://dev.epicgames.com/documentation/en-us/unreal-engine/exponential-height-fog-in-unreal-engine), [FExponentialHeightFogData](https://docs.unrealengine.com/4.26/API/Runtime/Engine/Components/FExponentialHeightFogData/index.html), [Adjust environment lighting features (tutorial values)](https://dev.epicgames.com/documentation/unreal-engine/07-adjust-environment-lighting-features)
- [Unreal: Local Fog Volumes](https://dev.epicgames.com/documentation/en-us/unreal-engine/local-fog-volumes-in-unreal-engine)
- [Hillaire 2015, Physically Based and Unified Volumetric Rendering in Frostbite](https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite) ([slides](https://www.slideshare.net/DICEStudio/physically-based-and-unified-volumetric-rendering-in-frostbite))
