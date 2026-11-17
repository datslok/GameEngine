# Clustered shading

Covers: `src/render/gpu/clusters`, `src/render/gpu/clusters_shared.h`, `assets/shaders/clusters.glsl`, `assets/shaders/frame_clusters.glsl`, `assets/shaders/cluster_lights.comp`

Each pixel walks only the lights that may reach its cluster, instead of every light in the frame, so a light costs only where it reaches and every light the buffers hold can be seated (`lights-and-shadow-atlas.md`). Maths in `render/gpu/clusters` (tested in `clusters_test`), mirrored in `clusters.glsl` (GPU-tested in `tests/gpu/clusters_gpu_test.cpp`); numbers in `clusters_shared.h`.

## The grid (`ClusterGrid`)

`makeClusterGrid(camera, width, height)` at the render size (throws for an empty screen).

| Part | Rule |
|---|---|
| Tiles | `clusterTilePixels` (64) square, last column and row cut by the screen's edge (2560 × 1440: 40 × 23) |
| Slices | `clusterDepthSlices` (24) per tile, exponential: slice k starts at near × (far / near)^(k / 24) (`sliceStartDepth`) |
| Space | cluster view space: x right, y up, z depth along the view, from the camera (`toClusterView`, the view matrix's rows) |
| A depth's slice | `floor(log(depth) * sliceScale + sliceBias)`, clamped; 0 at or behind the camera (`clusterSlice`) |
| Numbering | along a row, down the screen, then slice by slice (`clusterIndex`); a pixel from the top-left like `gl_FragCoord` (`clusterOfPixel`) |
| Bounds | `clusterBounds`: the box round the cluster's frustum piece (slice 0 starts at the camera), grown by `clusterBoundsMargin` (0.001) × its far depth, so a pixel rounding into a neighbour still finds its lights |

The grid reaches the shaders through `GpuFrameData` (`clusterTiles`, `clusterScreen`, `clusterSlicing`, view axes); `frameClusterGrid` rebuilds it there.

## The lists

- One bit per light per cluster: `clusterWords` (40) words, the point lights' `clusterPointWords` (32, for 1024) then the spotlights' `clusterSpotWords` (8, for 256), checked by `static_assert`. `clusterMaskBytes`: 160 bytes a cluster.
- A light's bit is set where its range sphere touches the cluster's box (`sphereTouchesBox`); a spotlight's cone must also touch the box's bounding sphere (`coneTouchesSphere`, Wronski 2016 "Cull that cone": the signed distance from the slanted edge, then past the range or behind the apex; cones of 90° or wider only range-tested). Both conservative.
- `buildClusterMasks(grid, lights)`: the CPU mirror of the whole pass. Tests: every light reaching a point is listed in that point's cluster (random lights round random points), and lights are listed only near where they reach.

## The pass (`cluster_lights.comp`, CLUSTERS)

- `clusterLightsPipeline`: a `ComputePipeline` on the scene's pipeline layout; one invocation per cluster, groups of `clusterGroupSize` (32), `workgroupCount`.
- Lights one word (32) at a time: each invocation turns one light into cluster view space into shared memory, `barrier()`, every invocation tests its cluster against the 32 and writes its own word, `barrier()`. Each light is read once per group; no two invocations write one word, so the result does not depend on order.
- Only words for the frame's light counts are written or read.
- Writes set 1 binding 11, `clusterMasks`: one GPU buffer sized with the frame images (`ensureFrameTextures`), shared by the frames in flight.
- `buildClusterLists`, after the debug UI and before shadows, between two `bufferBarrier`s: fragment and compute (no access: a write after a read needs only order) → compute storage write; then → fragment and compute storage read (the main pass and the fog's grid read the lists).
- Runs when `RenderSettings::clusteredShading` (on, live) or `clusterHeatmap` is on.

## Shading

In `scene_surface.glsl`, when `shadingOptions.x`:

- `firstClusterWord` finds the pixel's cluster; for each word up to the frame's counts, the set bits are walked lowest first (`findLSB`, then cleared), calling the same `addPointLight`/`addSpotLight` as the plain loops used with clustering off.
- Lowest bit first is slot order, and every reaching light is listed, so the pixel adds exactly the same lights in the same order: pictures are identical with clustering on and off.
- Each word is first OR-ed across the wave (`lightsOfTheWave`, `subgroupOr`), so pixels shaded together walk one list and read the same records at once. A neighbour's light cannot reach this pixel (it would be in its own list), so it adds nothing. `checkFormats` throws without fragment subgroup arithmetic.
- `fog_light.comp` walks the same lists for the fog's froxels (`fog.md`).

**Heatmap** (`RenderSettings::clusterHeatmap`, `shadingOptions.y`): three quarters of `clusterHeatColour(clusterLightCount())` over the pixel: black for none, then blue, green, yellow, red at `clusterHeatmapLights` (16) and above (`clusterHeatRamp`, branch-free clamped lines). Far, deep slices and grazing tiles list more lights than reach any one point; each extra listed light costs a pixel one distance check.
