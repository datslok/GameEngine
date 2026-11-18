# Occlusion culling

Covers: `src/render/gpu/occlusion_culling`, `src/render/gpu/occlusion_culling_shared.h`, `src/render/vulkan/vulkan_occlusion_culling`, `assets/shaders/occlusion_culling.glsl`, `assets/shaders/occlusion_count.comp`

Not drawing what hides behind other things. Research and sources: `docs/research/OCCLUSION.md`; stages: `ROADMAP.md` phase 18.

## Now: measured, not culled (`occlusion-measure`; GPU culling not worth it in the forest: `docs/research/OCCLUSION.md`)

- After the depth prepass (pass `OCCLUSION`, `renderer.md`): the prepass's depth reduced into a pyramid keeping the **farthest** depth (`VulkanDepthPyramid` with `DepthReduction::Farthest`, `reflections.md`), then `occlusion_count.comp` tests the camera's draws against it.
- Counts read back a frame or two late (as the depth range is) into `RenderStats::occlusion`; F4's `HIDDEN` line (`game-interface.md`). Drawing is unchanged.
- `RenderSettings::measureOcclusion` (on, `passes.measureOcclusion`); needs the prepass.
- Cost: the pyramid (R32F, full chain at the render size, about 20 MB at 2560 x 1440) and one dispatch.

## The test (`render/gpu/occlusion_culling`, mirrored in `occlusion_culling.glsl`)

| Step | Function | How |
|---|---|---|
| Sphere | instance's `levelSphere` | The level-of-detail sphere (a tree's parts share the tree's): larger than a part, so it can only under-count; radius below 0 never hidden |
| View | `makeOcclusionView` | Camera-relative view (`cameraRelativeView`), lens scales, near plane, depth = `depthConstant + depthPerInverseDistance / distance` from the GPU projection's depth row; TAA's jitter left out |
| Rectangle | `sphereRectFromView` | Mara & McGuire 2013's exact tangents, pixels as `ndcToPixel`, widened by `OCCLUSION_RECT_MARGIN` (1, covers jitter and rounding), clamped to the screen; none past the near plane |
| Nearest depth | `nearestDepthFromView` | Depth at the sphere's nearest distance along the view axis |
| Level | `occlusionLevel` | Longer side rounded up to a power of two: the rectangle touches at most 2 x 2 texels there |
| Hidden | `hiddenInPyramid` | The sphere's nearest depth strictly farther than the farthest of the cells under it (`pyramidCell`); sky (0) never hides |

- Conservative: whenever hidden, every pixel under the rectangle holds something nearer (`occlusion_culling_test` checks it on random depth images).
- The pyramid's odd edges: the last texel of an odd level also takes the leftover one below (`pyramidLastFine`), and `pyramidCell` clamps to it, so a cell covers exactly what was reduced into it.

## The counting pass (`VulkanOcclusionCulling`)

- Reads sets 0 and 1 (the scene: instances, the draw list, which compute can now read) and its own set: the batch table (`GpuOcclusionBatch`: first entry, count, the drawn level's triangles, per frame in flight in upload memory), the counts (`GpuOcclusionCounts`, GPU memory, cleared each frame), the pyramid.
- One workgroup of `OCCLUSION_GROUP_SIZE` (64) per batch, its instances shared out; sums in shared memory, then one atomic per count. At most `OCCLUSION_MAX_GROUPS` workgroups, each taking every that-many-th batch.
- Barriers as `VulkanDepthRange`'s (the counts buffer is shared by the frames in flight).

## Tests

- `occlusion_culling_test`: rectangles hold every surface point, nearest depth, near plane, off-screen spheres, the level, the pyramid never hiding what shows, a sphere behind a wall, batches, the readout line, the setting.
- `occlusion_culling_gpu_test` (`make gputest`): the GLSL against the mirror (rectangle corners within a pixel).
