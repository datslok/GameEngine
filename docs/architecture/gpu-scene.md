# GPU scene

Covers: `src/render/gpu/gpu_scene`, `src/render/gpu/still_instances`, `src/render/gpu/material_uniforms`, `src/render/gpu/instance_shared.h`, `src/render/vulkan/scene_layout`, `src/render/vulkan/bindless_textures`, `assets/shaders/instances.glsl`, `assets/shaders/frame_data.glsl`, `assets/shaders/materials.glsl`, `assets/shaders/triangle.vert`, `src/render/gpu/light_uniforms`, `src/render/gpu/material_shared`, `assets/shaders/scene_lights.glsl`

Every scene pass reads its objects, materials, lights and shadow tiles from storage buffers written once a frame, so nothing is bound or pushed per object and all objects sharing a mesh are one instanced draw per view. Staged plan for keeping more of it on the GPU: `docs/PERSISTENTSCENE.md`. How a frame uses it: `renderer.md`.

## Records

std430, mirrored byte for byte in the shaders, each size checked by a `static_assert`. Field meanings: the headers.

| Record | Bytes | Header | One per |
|---|---|---|---|
| `GpuInstance` | 208 | `gpu_scene.h` | `DrawItem` of the frame (instance i = draw i, the same for every view) |
| `GpuMaterial` | 272 | `material_uniforms.h` | distinct material, deduplicated by `MaterialTable` |
| `GpuDirectionalLight` | 64 | `light_uniforms.h` | seated directional light (`maxDirectionalLights` 4) |
| `GpuPointLight` | 48 | `light_uniforms.h` | seated point light (`maxPointLights` 1024) |
| `GpuSpotLight` | 128 | `light_uniforms.h` | seated spotlight (`maxSpotLights` 256), image lights' windows included |
| `ShadowTileData` | 112 | `render/gpu/shadow_map.h` | shadow tile (`lights-and-shadow-atlas.md`) |
| draw list entry | 4 | `instance_shared.h` | instance drawn in a view; top bit `DRAW_LIST_THINNED_BIT` = drawn thinned (`scene/card_thinning`) |
| `GpuThinningRecord` | 16 | `gpu_scene.h` | card thinning header and vertex |

- **Instance** (`packInstance(draw, material)`): world model matrix, normal matrix (`normalMatrix`), handedness and `uniformScale`, material and mesh numbers, terrain patch, instance kind (plain, impostor, follows ground), wind sway, hanging and thinning entries, the level-of-detail sphere. Each view multiplies in its own matrix on the GPU, so one record serves the camera, every shadow tile and ray tracing.
- **Lights:** `packLighting` and the slot rule (`selectDrawableLights`): `lights.md`. Shadows filled by `attachShadows` (`lights-and-shadow-atlas.md`).
- **Materials:** `MaterialTable::add` / `addPacked`: an FNV-1a hash of the packed bytes, then a full comparison (packing has no padding); the last 8 found checked first, since draws alternate few materials (bark, leaves).
- `packInstances` packs materials and instances on every core, handing out indices in draw order on one thread, so results match one-by-one adding.

## Uniform block and push constants

- **`GpuFrameData`** (432 bytes, std140, set 1 binding 0, vertex, fragment and compute): the camera-relative view-projection and `cameraOrigin`, ambient, light counts, the atlas texel size, the cluster grid (as `clusters.glsl`'s `ClusterGrid`), then option vectors each owned by a pass (probes, textures, mirrored light, AO, sky visibility, contact shadows, materials, levels). Made by `makeFrameData`; the renderer fills the options in `writeScene`. Stays well under the uniform limit (`src/render/CLAUDE.md`).
- **`GpuShadowView`** (112 bytes, vertex push constant): a tile's world view-projection, clip plane, light place (position w 1, sun direction w 0), options (x: casters sway).
- **`GpuCaptureView`** (96 bytes, `triangle.vert`'s push constant): a reflection probe face's or a mirror's camera-relative view-projection, clip plane and eye (w: thinning error a metre away). Passes that need none push an unused view.

## Batching and the draw list

- `batchByMesh(draws, chosen, firstInstance, levels)`: groups chosen draws by how they are drawn (one- or double-sided, solid or cut-out), then by mesh (handle order, each mesh's draws in chosen order: deterministic draw lists), into `DrawBatch {mesh, firstInstance, instanceCount, cutOut, doubleSided}`. With a `LevelView`, each draw uses the mesh its level of detail picks in that view; a draw picking nothing is left out. `BatchedDraws::thinned` marks draws using a thinned mesh.
- `VulkanRenderer::drawBatches(commands, batches, BatchPipelines{plain, cutOut, cull})`: binds the style's pipeline when it changes (cut-outs last, so once), sets the cull mode when it changes (`vkCmdSetCullMode`; scene pipelines have `dynamicCullMode`; none for double-sided), binds the mesh and calls `vkCmdDrawIndexed(indexCount, instanceCount, 0, 0, firstInstance)`, so `gl_InstanceIndex` is the draw list entry.
- One draw list per frame, in order: main pass, each redrawn shadow tile's casters (no thinned bit: shadow shaders thin nothing), velocity pass, reflection probe faces, mirror view, transparent draws. `appendDrawList` writes a view's entries with their thinned bits.

## Shaders

- Declaration-only includes: `instances.glsl` (`InstanceData`, `Instances` binding 1, `DrawList` binding 10), `frame_data.glsl` (`FrameData`, binding 0), `materials.glsl` (the bindless array and `MaterialData`), `scene_lights.glsl` (light records, bindings 3-5, shared with `cluster_lights.comp`); `frame_clusters.glsl` builds `ClusterGrid` from the block.
- `triangle.vert`: reads `instances[drawnInstance(drawList[gl_InstanceIndex])]`, subtracts `cameraOrigin` from the model's translation (camera-relative: `depth.md`), then terrain, card thinning, ground following and wind sway; passes the material number flat.
- `scene_surface.glsl` (the main pass's shading): `materials[index]`, textures as `textures[nonuniformEXT(slot)]` (neighbouring pixels may be different objects), each light's shadow from its record.
- `shadow.vert`: `tileMatrix * (model * position)` in world space, so cached tiles stay keyed on world matrices.

## Descriptors (`scene_layout`)

**Set 0, `BindlessTextures`:** one binding of 16,384 combined image samplers (or the device's update-after-bind limit if lower), all with the surface sampler; update-after-bind, partially bound, update-unused-while-pending, so `uploadTexture` writes slot handle + 1 while frames using other slots are in flight; slot 0 the white stand-in (`bindlessTextureSlot`). `checkTexturesUploaded` throws for a material texture never uploaded.

**Set 1, the frame's scene** (`SceneBinding`, 43 bindings), written once a frame by `writeSceneSet` from the frame's descriptor pools. Absent features bind stand-ins, so the layout never changes.

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

**Sharing:** `SceneLayout` owns set 1's layout and one pipeline layout of both sets plus the push constant, shared by the main, prepass, shadow, tile reset, velocity and coloured-shadow pipelines and the cluster pass (`sharedLayout`). `bindScene` binds both sets once per pass (graphics by default; compute for the cluster pass) and they stay bound across those pipelines. The glass ray pipelines add the ray tracing set; transparent pipelines add a set 2 of their own.

## Buffers (`SceneBuffers`)

- One per frame in flight: `UploadArray`s in upload memory (frame data, instances, materials, three light kinds, shadow tiles, draw list, probe hit lights, previous models, sky light, atmosphere, mirror pairs, glass bounds, fog, sky visibility cascades, terrain patches, wind, wind anchors).
- Each grows (to twice its size or what is needed) when a frame needs more; safe because the frame's fence has passed.
- `UploadArray::write(first, second)` writes kept and new records back to back.
- Records that change rarely (terrain heights, hanging vertices, thinning records) live in GPU memory, copied only when they change.

## Still instances (`render/gpu/still_instances`)

The GPU side of still draws (`StillDraws`, `ModelRenderer::still`: `game-interface.md`).

- `StillInstances` (owned by `VulkanRenderer`) keeps the still draws' instances, last-frame matrices and their own `MaterialTable`.
- `update` repacks only `builtStillDraws`, and only when the frame follows the last one packed (`stillVersion`, `stillPreviousVersion`); otherwise (another count, a whole build, a skipped frame) it packs all; the same frame twice (capture warm-up) packs nothing.
- Stale table entries are dropped by a whole repack once the table holds more than twice its whole packing's entries plus 64.
- `packMovingDraws`: the moving draws' instances and matrices, their materials added to a copy of the still table, which becomes the frame's.
- Textures are checked only where something was packed. Tested in `still_instances_test`.
