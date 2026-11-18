# Ray tracing

Covers: `src/render/gpu/ray_tracing`, `src/render/gpu/ray_tracing_shared.h`, `src/render/vulkan/vulkan_ray_tracing`, `assets/shaders/ray_tracing.glsl`, `assets/shaders/ray_scene.glsl`, `assets/shaders/ray_debug.frag`

Hardware ray queries: any shader asks what a ray hits first (distance, instance, triangle, barycentrics, front or back). Rules: `src/render/CLAUDE.md` (every ray-traced feature under the master switch). Light probes, the biggest user: `ddgi.md`.

## Optional and off by default

- **Device:** `VulkanDevice` enables `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` when present (`supportsRayQueries`, `getScratchAlignment`). Without them `VulkanRenderer::rayTracing` is null and nothing traces.
- **Master switch:** `RenderSettings::rayTracing` (F5 Passes, `engine.render.passes.rayTracing`, live, off by default; presets never touch it). `tracesRays(settings, rayQueries)` = switch on and GPU capable; asked once a frame.
- **Gated features**, each keeping its own switch, read only under the master one:

| Feature | Its switch |
|---|---|
| Light probes | `GlobalIlluminationSettings::enabled` (`VulkanRenderer::tracesProbes`) |
| Ray-traced reflections | `ReflectionSettings::rayTraced` |
| Reflection probe capture tracing the sun's shadow past its cascades | none (`probe_capture_rays.frag` vs `probe_capture.frag`, both kept, `capturePipelineIndex`) |
| Mirrored lights | `MirroredLightSettings::enabled` |
| Tinted shadows through glass | `TransparencySettings::glassShadows` |
| Debug view | `RenderSettings::rayDebugView` |

- Bottom-level structures are built at upload whenever the GPU can, so flipping the switch rebuilds nothing; the top level is built only on frames where one of the above traces.

## Buffer device addresses

Required by Vulkan 1.3 and used here as pointers for builds and shader reads.

- `VulkanBuffer::getDeviceAddress`, `UploadArray::getDeviceAddress`; VMA's `BUFFER_DEVICE_ADDRESS` flag.
- Every mesh buffer (the shared mesh buffer's pools, a deforming mesh's own) has `SHADER_DEVICE_ADDRESS` usage (plus build-input usage with ray tracing): `getVertexAddress`, `getIndexAddress` (a pooled mesh's start in the pools), `getVertexCount`. `meshUploadBarrier` covers fragment and compute storage reads and structure builds. `MeshStructure::source` keeps the mesh, so the mesh table reads its addresses each frame (a pool moves when it grows).

## API-free part

`render/gpu/ray_tracing`, numbers in `ray_tracing_shared.h`, mirrored by `assets/shaders/ray_tracing.glsl`, tested in `ray_tracing_test` and `ray_tracing_gpu_test`.

**Masks** (`rayMaskFor(draw)`):

| Mask | Bit | Who carries it |
|---|---|---|
| `solidRayMask` | 1 | Shadow casters: block light, seen by light-gathering rays |
| `visualOnlyRayMask` | 2 | The rest (lights' glowing stand-ins, bulbs, discs): already counted as direct light |
| `staticRayMask` | 4 | Static shadow casters (`DrawItem::isStatic`), in addition to solid: probe relocation sees only the level |
| `glassRayMask` | 8 | Glass and blended surfaces, alone: only `glassTransmittance`'s rays see them, tinting light per pane |
| `everythingRayMask` | 255 | Reflections, the debug view |

**Instances:** `GpuRayInstance` (64 bytes, `VkAccelerationStructureInstanceKHR`'s layout, `static_assert`ed in `vulkan_ray_tracing.cpp`) from `packRayInstance(draw, instanceNumber, meshStructure, flags)`.

- Model's top three rows row-major; instance number (24 bits, `maxRayInstances`) under the mask; record offset under the flags; the mesh structure's address.
- Cut-outs and glass get `rayInstanceNotOpaque`: every hit is a candidate the shader tests (texel alpha, or tint and pass over).
- Instance i is the frame's draw i, so a hit finds its `GpuInstance`; `GpuInstance::material[1]` holds the mesh number, so it finds its triangle's corners.

**Facing:** `hitsFrontFace(a, b, c, direction, handedness)`: front = corners anticlockwise seen from the ray. The GPU decides in the instance's own space, so mirrored instances keep their outside as front with no instance flags.

**Camera rays:** `GpuRayDebugView` (80 bytes of push constants: right and up scaled by half-view tangents, forward, camera position with the view in w, screen size, z whether the sky draws sun and moon discs: `skyWithDiscs`, `skyWithoutDiscs`) from `packRayDebugView`; also the sky pass's ray layout. `cameraRayDirection`, `hitCornerWeights` mirrored.

## Acceleration structures

`VulkanRayTracing` (`render/vulkan/vulkan_ray_tracing`), made by `VulkanRenderer::createRayTracing`.

**Bottom level, one per mesh** (`addMesh`, from `VulkanRenderer::uploadMesh`; meshes numbered in upload order like their handles):

- Opaque triangle geometry from vertex and index addresses, 48-byte stride, `PREFER_FAST_TRACE | ALLOW_COMPACTION`.
- Compacted: size read by a `COMPACTED_SIZE` query after the build, copied with `COMPACT` into a buffer that size, original dropped.
- Barrier to top-level builds and ray queries in later submissions.
- **Deforming meshes:** one structure per slot of the mesh's ring (`deforming_slots`), `PREFER_FAST_BUILD`, not compacted; `rebuildDeformedMeshes` rebuilds a slot whose shape is newer than its structure, before `buildScene`. The ring keeps the slot out of the frames in flight's reach.
- Scratch is allocated one alignment larger and its address rounded up (`createScratch`, `scratchAddress`).

**Top level, one per frame in flight** (with its own scratch, instance and mesh-table `UploadArray`s and descriptor set; `setFramesInFlight` remakes them between frames):

- `packInstances(draws)`: every draw but the cells' empty slots (`isEmptyDrawSlot`), throwing for a mesh without a structure.
- `buildScene(commands, frameIndex, instances)`: writes mesh table and instances, grows the structure (from 256 instances, doubling or to what is needed), rebuilds from scratch every frame (never stale, no bookkeeping for movers), barriers to fragment and compute ray queries, rewrites the set.
- Timed as `GpuPass::RayScene` (TLAS in F4).

**The set** (`ray_scene.glsl`, set 2 unless `RAY_SCENE_SET` says otherwise):

| Binding | Holds |
|---|---|
| 0 | The frame's top-level structure |
| 1 | Mesh table: `{vertices, indices}` addresses per mesh |

- Buffers by address with `GL_EXT_buffer_reference_uvec2` (addresses as two 32-bit halves: no 64-bit integers needed).
- Vertices read as a float array of `MESH_VERTEX_FLOATS` (12) per vertex, since std430 would pad a struct of vec3s; offsets `MESH_VERTEX_POSITION`, `_NORMAL`, `_UV`, `_TANGENT`.
- `meshSurfaceAt(mesh, triangle, barycentrics)`: interpolated normal and UV in the mesh's own space.
- `glassTransmittance` and `lightThrough` (tinted light through glass) live here too.

## Debug view

`RenderSettings::rayDebugView` (`RayDebugView`: Off, Normals, Albedo, Faces; F5 Passes; Off by default; only under the master switch).

- After the main pass, `drawRayDebugView` draws `ray_debug.frag` over the scene image, its colour discarded and redrawn (`GpuPass::RayDebug`, RAYS).
- Layout `rayDebugLayout`: the scene's two sets, the ray tracing set, the camera as push constants.
- One ray per pixel against every mask bit:

| View | Shows |
|---|---|
| Normals | World normal of the hit (through the instance's normal matrix) |
| Albedo | Colour texture's full-size level times base colour (`materials.glsl`) |
| Faces | Front green, back red, shaded by angle |
| (miss) | Dark grey |
