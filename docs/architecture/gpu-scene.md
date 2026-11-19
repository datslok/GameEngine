# GPU scene

Covers: `src/render/gpu/gpu_scene`, `src/render/gpu/still_instances`, `src/render/gpu/lasting_buffers`, `src/render/vulkan/vulkan_lasting_buffer`, `src/render/vulkan/vulkan_indirect_commands`, `src/render/gpu/mesh_pool`, `src/render/vulkan/vulkan_mesh_pool`, `src/render/vulkan/vulkan_mesh`, `src/render/gpu/material_uniforms`, `src/render/gpu/instance_shared.h`, `src/render/vulkan/scene_layout`, `src/render/vulkan/bindless_textures`, `assets/shaders/instances.glsl`, `assets/shaders/instance_data.glsl`, `assets/shaders/frame_data.glsl`, `assets/shaders/materials.glsl`, `assets/shaders/triangle.vert`, `src/render/gpu/light_uniforms`, `src/render/gpu/material_shared`, `assets/shaders/scene_lights.glsl`

Every scene pass reads its objects, materials, lights and shadow tiles from storage buffers written once a frame, so nothing is bound or pushed per object and all objects sharing a mesh are one instanced draw per view. Staged plan for keeping more of it on the GPU: `docs/research/PERSISTENTSCENE.md`. How a frame uses it: `renderer.md`.

## Records

std430, mirrored byte for byte in the shaders, each size checked by a `static_assert`. Field meanings: the headers.

| Record | Bytes | Header | One per |
|---|---|---|---|
| `GpuInstance` | 208 | `gpu_scene.h` | `DrawItem` of the frame (instance i = draw i, the same for every view) |
| `GpuMaterial` | 320 | `material_uniforms.h` | distinct material, deduplicated by `MaterialTable` |
| `GpuDirectionalLight` | 64 | `light_uniforms.h` | seated directional light (`maxDirectionalLights` 4) |
| `GpuPointLight` | 48 | `light_uniforms.h` | seated point light (`maxPointLights` 1024) |
| `GpuSpotLight` | 128 | `light_uniforms.h` | seated spotlight (`maxSpotLights` 256), image lights' windows included |
| `ShadowTileData` | 112 | `render/gpu/shadow_map.h` | shadow tile (`lights-and-shadow-atlas.md`) |
| draw list entry | 4 | `instance_shared.h` | instance drawn in a view (low 24 bits); top bit `DRAW_LIST_THINNED_BIT` = drawn thinned (`scene/card_thinning`); 7 bits between, its level fade (`DRAW_LIST_FADE_*`, level-of-detail.md) |
| `GpuThinningRecord` | 16 | `gpu_scene.h` | card thinning header and vertex |

- **Instance** (`packInstance(draw, material)`): world model matrix, normal matrix (`normalMatrix`), handedness and `uniformScale`, material and mesh numbers, terrain patch, instance kind (plain, impostor, follows ground), wind sway, hanging and thinning entries, the level-of-detail sphere. Each view multiplies in its own matrix on the GPU, so one record serves the camera, every shadow tile and ray tracing.
- **Lights:** `packLighting` and the slot rule (`selectDrawableLights`): `lights.md`. Shadows filled by `attachShadows` (`lights-and-shadow-atlas.md`).
- **Materials:** `MaterialTable::add` / `addPacked`: an FNV-1a hash of the packed bytes, then a full comparison (packing has no padding); the last 8 found checked first, since draws alternate few materials (bark, leaves). A table `over` a base finds the base's entries first and numbers its own after them.
- `packInstances` packs materials (a blended ground's layers in their own list) and instances on every core, marks each draw whose packed bytes equal the draw before's (the terrain's patches of one region), and hands out indices in draw order on one thread only where they differ, so results match one-by-one adding (`add`'s ground checks kept, `checkBlendedGround`). Ground draws pack in pieces of 32 (five materials each).

## Uniform block and push constants

- **`GpuFrameData`** (432 bytes, std140, set 1 binding 0, vertex, fragment and compute): the camera-relative view-projection and `cameraOrigin`, ambient, light counts, the atlas texel size, the cluster grid (as `clusters.glsl`'s `ClusterGrid`), then option vectors each owned by a pass (probes, textures, mirrored light, AO, sky visibility, contact shadows, materials, levels). Made by `makeFrameData`; the renderer fills the options in `writeScene`. Stays well under the uniform limit (`src/render/CLAUDE.md`).
- **`GpuShadowView`** (112 bytes, vertex push constant): a tile's world view-projection, clip plane, light place (position w 1, sun direction w 0), options (x: casters sway).
- **`GpuCaptureView`** (96 bytes, `triangle.vert`'s push constant): a reflection probe face's or a mirror's camera-relative view-projection, clip plane and eye (w: thinning error a metre away). Passes that need none push an unused view.

## The shared mesh buffer

Every mesh that does not deform lives in two pools (`VulkanMeshPool`, research `docs/research/MESHPOOL.md`): `MeshVertex`es in one GPU buffer, `uint32` indices in another, each mesh a run of each (`MeshPlacement`), its indices its own (the draw's `vertexOffset` adds its first vertex).

- **Added only**, between frames (`uploadMesh`): `MeshPoolLayout::place` after the last; a pool too small is doubled (`poolCapacity`, from 1M vertices and 4M indices, under 4 GB), the GPU waited for, the old one copied over on the GPU.
- `VulkanMesh` with the pool keeps only its placement: its buffers are the pools', its addresses the pools' plus its start. Deforming meshes (cloth) keep buffers of their own (`pooled` false), drawn alone.
- **Vertex shaders** read per-vertex records by `gl_VertexIndex - gl_BaseVertexARB` (the mesh's own vertex number; `shaderDrawParameters`).
- F4's VRAM line: `MESHES used OF held MB`.

## Batching and the draw list

- `batchByMesh(draws, chosen, firstInstance, levels)`: groups chosen draws by how they are drawn (one- or double-sided, solid or cut-out), then by mesh (handle order, each mesh's draws in chosen order: deterministic draw lists), into `DrawBatch {mesh, firstInstance, instanceCount, cutOut, doubleSided}`. With a `LevelView`, each draw uses the mesh its level of detail picks in that view; a draw picking nothing is left out. `BatchedDraws::thinned` marks draws using a thinned mesh.
- `VulkanRenderer::drawBatches(commands, batches, BatchPipelines{plain, cutOut, cull})`: `drawRuns` joins consecutive pooled batches drawn the same way; each run binds its style's pipeline and cull mode when they change (`vkCmdSetCullMode`; scene pipelines have `dynamicCullMode`; none for double-sided), the pools once (`bindPools`), and draws by one `vkCmdDrawIndexedIndirect` of its commands side by side (`packDrawCommand(batch, placement)`: `GpuDrawCommand`, byte for byte `VkDrawIndexedIndirectCommand`, the mesh's `firstIndex` and `vertexOffset`, the batch's instances from `firstInstance`), written by `VulkanIndirectCommands::writeRun` (upload memory in 4096-command chunks, a run in one chunk, reset when the frame's fence passes, flushed at submit). A mesh with its own buffers is bound and drawn alone. `gl_InstanceIndex` is the draw list entry (`drawIndirectFirstInstance`), several draws a call need `multiDrawIndirect`. The camera's passes go through `drawPassDraws` (`PassDraws`): these batches, or with GPU culling the GPU's packed commands (`gpu-driven.md`).
- One draw list per frame, in order: main pass, each redrawn shadow tile's casters (no thinned bit: shadow shaders thin nothing), velocity pass, reflection probe faces, mirror view, transparent draws. The CULL pass writes every view's regions, each room for every draw; the CPU writes only glass's entries (`DrawListLayout`, `gpu-driven.md`). `appendDrawList` gives a CPU list's entries with their thinned bits (cull-compare's reference).

## Shaders

- Declaration-only includes: `instances.glsl` (`InstanceData` from `instance_data.glsl`, `Instances` binding 1, `DrawList` binding 10), `frame_data.glsl` (`FrameData`, binding 0), `materials.glsl` (the bindless array and `MaterialData`), `scene_lights.glsl` (light records, bindings 3-5, shared with `cluster_lights.comp`); `frame_clusters.glsl` builds `ClusterGrid` from the block.
- `triangle.vert`: reads its instance through `drawnObject` (`drawn_objects.glsl`): `instances[drawnInstance(drawList[gl_InstanceIndex])]`, or for a draw from `PLACED_INSTANCE_BASE` a placed part built from its plant (`planting.md`); subtracts `cameraOrigin` from the model's translation (camera-relative: `depth.md`), then terrain, card thinning, ground following and wind sway; passes the material number flat.
- `scene_surface.glsl` (the main pass's shading): `materials[index]`, textures as `textures[nonuniformEXT(slot)]` (neighbouring pixels may be different objects), each light's shadow from its record.
- `shadow.vert`: `tileMatrix * (model * position)` in world space, so cached tiles stay keyed on world matrices.

## Descriptors (`scene_layout`)

**Set 0, `BindlessTextures`:** one binding of 16,384 combined image samplers (or the device's update-after-bind limit if lower), all with the surface sampler; update-after-bind, partially bound, update-unused-while-pending, so `uploadTexture` writes slot handle + 1 while frames using other slots are in flight; slot 0 the white stand-in (`bindlessTextureSlot`). `checkTexturesUploaded` throws for a material texture never uploaded.

**Set 1, the frame's scene** (`SceneBinding`, 46 bindings), written once a frame by `writeSceneSet` from the frame's descriptor pools. Absent features bind stand-ins, so the layout never changes.

| Bindings | Holds | Owner |
|---|---|---|
| 0-2 | frame data, instances, materials | here |
| 3-5 | directional, point, spot lights | `lights.md` |
| 6-8 | shadow tiles, atlas with compare sampler, atlas raw | `lights-and-shadow-atlas.md` |
| 9 | BRDF table | `shading.md` |
| 10 | draw list | here |
| 11 | cluster masks | `clustered-shading.md` |
| 12-16, 20 | probe atlases, hit lights, fast atlas, states, cascades | `ddgi.md` |
| 17-19 | flat ambient, sky view, atmosphere | `sky.md` |
| 21 | last frame's model matrices | `anti-aliasing.md` |
| 22-24 | mirrored light images and pairs | `mirrored-lights.md` |
| 25 | ambient occlusion | `ambient-occlusion.md` |
| 26, 33, 34 | glass bounds, coloured shadow maps (tint, depth) | `glass.md` |
| 27, 28 | fog record and volume | `fog.md` |
| 29, 30 | sky visibility grid and cascades | `sky-visibility.md` |
| 31, 32 | contact shadow masks | `sun-shadows.md` |
| 35, 36 | terrain heights and patches | terrain (`scene/terrain`) |
| 37-41 | wind field, springs (two), hanging vertices, anchors | wind (`render/gpu/wind`) |
| 42 | card thinning records | card thinning (`scene/card_thinning`) |
| 43-45 | the planting cells' slots' plants, placed parts' instances, the camera's placed entries | `planting.md` (Placed plants drawn) |

**Sharing:** `SceneLayout` owns set 1's layout and one pipeline layout of both sets plus the push constant, shared by the main, prepass, shadow, tile reset, velocity and coloured-shadow pipelines and the cluster pass (`sharedLayout`). `bindScene` binds both sets once per pass (graphics by default; compute for the cluster pass) and they stay bound across those pipelines. The glass ray pipelines add the ray tracing set; transparent pipelines add a set 2 of their own.

## Buffers (`SceneBuffers`)

- One per frame in flight: `UploadArray`s in upload memory (frame data, three light kinds, shadow tiles, draw list, probe hit lights, sky light, atmosphere, mirror pairs, glass bounds, fog, sky visibility cascades, terrain patches, wind, wind anchors).
- Each grows (to twice its size or what is needed) when a frame needs more; safe because the frame's fence has passed.
- Records that change rarely (terrain heights, hanging vertices, thinning records) live in GPU memory, copied only when they change.

## Lasting buffers (`render/gpu/lasting_buffers`, `render/vulkan/vulkan_lasting_buffer`)

Instances, materials and last-frame matrices (bound through `SceneBufferViews`), and the culling's records and candidates (`gpu-driven.md`), live in GPU memory every frame in flight reads, written only where they change (GPU-driven drawing's stage 7a; research `docs/research/GPUDRIVEN.md`).

| Piece | What |
|---|---|
| `LastingWrite` | records for a buffer from record `first` on; `appendLastingWrites` makes them from ranges over a span (joined, `joinDrawRanges`) |
| `planLastingCopies` | the writes staged one after another, a copy each, joined where both sides follow on |
| `VulkanLastingBuffer::reserve` | room for n records (`lastingCapacity`: twice, or what is needed, at least 64); true when made afresh (then the caller writes everything); waits for the GPU first |
| `write` | stages in the frame in flight's own upload buffer, records `vkCmdCopyBuffer` between two barriers: every earlier command (earlier frames' reads, first in the queue) before the copy, the copy before every later storage read |

- **Instances, last-frame matrices** (`writeLastingRecords`): the still records `StillInstances` repacked (all when a buffer was made afresh), then the moving draws' after them every frame.
- **Materials:** the still table's entries from `getFirstNewMaterial`, then the moving draws' table (`MaterialTable::over` the still one: only materials it lacks, numbered after it).
- **Standing still** copies only the moving draws' records; F4's `UPLOADED` shows the bytes (with the culling's).
- Tested in `lasting_buffers_test` (planning, ranges, capacity); the copies themselves by eye.

## Still instances (`render/gpu/still_instances`)

The GPU side of still draws (`StillDraws`, `ModelRenderer::still`: `game-interface.md`).

- `StillInstances` (owned by `VulkanRenderer`) keeps the still draws' instances, last-frame matrices and their own `MaterialTable`.
- `update` repacks only `builtStillDraws`, and only when the frame follows the last one packed (`stillVersion`, `stillPreviousVersion`) and its ranges cover every slot added at the end (a cell's new block); otherwise (fewer slots, uncovered new ones, a whole build, a skipped frame) it packs all; the same frame twice (capture warm-up) packs nothing.
- Stale table entries are dropped by a whole repack once the table holds more than twice its whole packing's entries plus 64.
- `packMovingDraws`: the moving draws' instances and matrices, their materials in a table over the still one (`MaterialTable::over`; `at` looks a number up in either).
- Textures are checked only where something was packed. Tested in `still_instances_test`.
