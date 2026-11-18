# Renderer

Covers: `src/render/vulkan/vulkan_renderer`, `src/render/vulkan/vulkan_device`, `src/render/vulkan/vulkan_swapchain`, `src/render/vulkan/swapchain_choice`, `src/render/vulkan/image_state`, `src/render/vulkan/buffer_barrier`, `src/render/vulkan/vulkan_memory`, `src/render/vulkan/owned_handle.h`, `src/render/vulkan/vulkan_check`, `src/render/vulkan/immediate_commands`, `src/render/vulkan/vulkan_pipeline`, `src/render/vulkan/pipeline_compiler`, `src/render/vulkan/frame_descriptors`, `src/render/vulkan/uniform_ring`, `src/render/vulkan/descriptor_helpers`, `src/render/vulkan/frame_context.h`, `src/render/vulkan/render_pass`, `src/render/vulkan/vulkan_mesh`, `src/render/vulkan/vulkan_texture`, `src/render/gpu/depth_prepass`, `src/render/gpu/uniform_limits.h`, `src/render/gpu/column_major.h`, `src/render/gpu/loading_screen`, `src/render/frames_in_flight.h`, `src/render/present_mode.h`, `src/render/mesh_upload`, `src/render/software/`, `src/render/gpu/render_settings`, `src/render/gpu/mip_levels`, `src/render/gpu/compute_dispatch`, `src/render/vulkan/vulkan_compute`, `assets/shaders/triangle.frag`, `assets/shaders/triangle.vert`, `assets/shaders/triangle_rays.frag`, `assets/shaders/fullscreen_triangle.vert`

`VulkanRenderer`: the engine's own Vulkan 1.3 renderer, drawing a `FrameDescription` into a window it does not own. Rules: `src/render/CLAUDE.md`, `src/render/vulkan/CLAUDE.md`. Libraries and validation layers: `build.md`. The scene's records: `gpu-scene.md`; lights and the shadow atlas: `lights-and-shadow-atlas.md`; per-pass timing: `gpu-timing.md`; depth precision: `depth.md`.

## Public calls

| Call | Does |
|---|---|
| constructor (`SDL_Window*`, `JobSystem&`, validation, frames in flight) | Device, layouts, every pipeline submitted to the compiler |
| `pipelineProgress`, `finishPipelines`, `drawLoadingScreen` | The loading screen (below) |
| `render(frame, settings, ui)` | One frame to the screen; false while minimised or out of date |
| `capture` | A frame into an image read back (golden images), after warm-up frames (`drawFramesBeforeCapture`) |
| `uploadMesh`, `uploadTexture`, `uploadCompressedTexture` (BC7, when `supportsBlockCompression`), counts | GPU copies in arrays indexed exactly like the handles |
| `startDebugUi`, `beginDebugUiFrame`, `stopDebugUi` | ImGui's Vulkan backend |
| `setPresentMode`, `setFramesInFlight` and getters | Applied live by `Application` |
| `getLastFrameStats`, `getDisplayOutput`, `getFrameAspectRatio`, `getDeviceName` (the golden folder's name), `supportsRayQueries`, `upscalerSupport` | Queries |
| `setClothWork`, `takeClothReadbacks`, `setClothPassTiming` | GPU cloth (`cloth-gpu.md`) |

It never reads files. Its camera copy takes the acquired frame's aspect ratio; `Application` copies it back into the game's camera for click rays.

## A frame (`render`)

1. `updateDisplayOutput`: the window's HDR properties every frame; `swapchain.supportsHdr()` asked only when HDR comes on (`image-pipeline.md`).
2. `prepareSwapchain`: rebuild after `waitIdle` when missing, out of date, HDR or present mode or frames in flight changed; HDR refused → SDR, marked unsupported; screen pipelines (tone mapping, lines, overlay) remade for a new format. False while minimised (10 ms sleep).
3. `bakeSkyVisibility` on the CPU (`sky-visibility.md`), before the fence wait.
4. `beginFrame`: wait for the frame's fence; read back its timestamps, cloth stamps, depth ranges, eye adaptation and cloth points; reset its descriptor pools, uniforms and command pool; reset its query pool.
5. Acquire (`imageAcquired`). Out of date: submit the empty buffer, draw nothing.
6. `ensureFrameTextures`: scene colour, depth and solid mask at the render size (`renderSize`; DLSS's own size when it upscales), the cluster masks, and every screen-sized pass's images, after `waitIdle`.
7. `drawScene` (pass table below).
8. Swapchain image `Acquired` → `ColourTarget` (discarded); one rendering block: tone mapping, debug lines, overlay composite; → `Present`.
9. `submitFrame`: waits for `imageAcquired` at `acquireWaitStage` (colour output), signals the image's render-finished semaphore (one per swapchain image), resets the fence only now, presents.

An exception mid-frame: `shadowMemory.forgetAll()`, then `abandonFrame` ends an open rendering block, moves an acquired image to `Present`, submits and presents (the swapchain lent the image).

## drawScene: CPU first

Everything every view draws is settled before any command is recorded, so the scene buffers are written once (`writeScene`).

1. Cloth ticks (`runCloths`), video memory read.
2. Camera: `viewProjection` (world), `relativeViewProjection` (camera-relative, jittered for TAA), `motionViewProjection` (unjittered), last frame's for reprojection; `makeClusterGrid`.
3. Directional lights dimmed by the air when there is an `Atmosphere` (so the brightest *arriving* light gets the cascades), then `prioritizeLights`.
4. `CameraCullTable::update`; only the glass and the flat-faced draws (the mirror's candidates) tested on the CPU. The flat mirror chosen; image lights added without ray-traced mirror search.
5. `packLighting`, `planShadows`, `holdStaggeredCascades`, `attachShadows`; coloured shadow maps made if glass needs them.
6. The frame's draw list laid out (`DrawListLayout`): the camera's region, `prepareShadowDraws` (which tiles to draw, their regions), the velocity pass's region, the reflection probe's faces' and the mirror's, then transparent draws sorted far to near.
7. Probe cascades, mirrored light pairs, ambient occlusion, contact shadows, fog, wind and sky visibility readied before the scene set is written.
8. `writeScene`, debug line vertices and shadow tile data straight into upload memory (no copy passes).

Draw list order (one buffer): main pass, shadow tiles, velocity pass, probe faces, mirror view, transparent. The main and velocity regions, each probe face's and the mirror's, and each drawn shadow tile's hold every draw, written by the CULL pass (`gpu-driven.md`); the CPU writes only glass's entries.

## Pass order

Each row is a `GpuPass` (F4 name in brackets); skipped passes cost two equal stamps.

| # | Pass | Runs when | Owner doc |
|---|---|---|---|
| 1 | Cloth [CLOTH] | GPU cloths ticked | `cloth-gpu.md` |
| 2 | Debug UI [UI] | F5 open: the UI into its own overlay image | `settings-and-commands.md` |
| 3 | Culling [CULL] | always: the camera's, velocity pass's, probe faces', mirror's and drawn shadow tiles' lists and commands | `gpu-driven.md` |
| 4 | Ray scene [TLAS] | anything traces rays (probes, ray debug, probe capture, reflections, mirrored lights, glass shadows) | `ray-tracing.md` |
| 5 | Atmosphere [SKY] | frame has an `Atmosphere` | `sky.md` |
| 6 | Probes [PROBES] | probes traced (`tracesProbes`) | `ddgi.md` |
| 7 | Wind [WIND] | the wind field runs | wind (`render/gpu/wind`) |
| 8 | Clusters [CLUSTERS] | `clusteredShading` or `clusterHeatmap` | `clustered-shading.md` |
| 9 | Shadows [SHADOW] | any tile redrawn; coloured shadow maps after | `lights-and-shadow-atlas.md`, `glass.md` |
| 10 | Fog [FOG] | frame has a `Fog` and fog is on | `fog.md` |
| 11 | Depth prepass [PREPASS] | `depthPrepass` (on) | here |
| 12 | Colour pyramid [PYRAMID] | AO gathers light from last frame's picture | `ambient-occlusion.md` |
| 13-16 | AO, AOHISTORY, AODENOISE, AOUPSAMPLE | ambient occlusion on (needs the prepass) | `ambient-occlusion.md` |
| 17 | Contact shadows [CONTACT] | sun or lamp contact shadows on (needs the prepass) | `sun-shadows.md` |
| 18 | Mirrored lights [MIRROR] | rays, prepass, a fast light on a smooth surface | `mirrored-lights.md` |
| 19 | Main [MAIN] | always; sky and stars drawn in it or in a block of their own after | here |
| 20 | Probe balls [BALLS], ray debug [RAYS] | debug views | `ddgi.md`, `ray-tracing.md` |
| 21 | Motion [MOTION] | temporal AA: moved or swaying draws into the velocity image | `anti-aliasing.md` |
| 22 | Depth range [DEPTHS] | `fitCascadesToDepth` | `sun-shadows.md` |
| 23 | Reflection probe [CUBE], planar reflection [PLANAR] | reflections on | `reflection-probe.md`, `planar-reflections.md` |
| 24 | Reflections [SSR] | reflections on | `reflections.md` |
| 25 | Fog apply [FOGMIX] | fog | `fog.md` |
| 26 | Transparent [GLASS] | glass in view (picture before it kept, reactive mask) | `glass.md` |
| 27 | Anti-aliasing [AA] | TAA, FSR or DLSS | `anti-aliasing.md` |
| 28 | Depth of field [DOF], motion blur [BLUR] | camera settings | `camera-effects.md` |
| 29 | Eye adaptation [EYE] | `eyeAdaptation` | `image-pipeline.md` |
| 30 | Bloom [BLOOM] | always (`VulkanBloom`) | `image-pipeline.md` |
| 31 | Tone map [TONE], debug lines [LINES], overlay [OVERLAY] | in the swapchain's block | `image-pipeline.md`, `game-interface.md`, `settings-and-commands.md` |

The F4 readout groups them: setup to the prepass; lighting from `firstLightingPass` (PYRAMID) to MAIN; picture from `firstPicturePass` (BALLS) on (`engine/debug_views.h`).

## Depth prepass and main pass

**Prepass** (`drawDepthPrepass`, `render/gpu/depth_prepass`):

- Depth `DepthTarget` (discarded, cleared to far, stored); the solid mask (`R8_UNORM`) cleared to 1.
- `depthPrepassPipeline`: the main pass's `triangle.vert` and vertex layout, no fragment shader, colour writes off; `GREATER`, written (`prepassDepthState`).
- `depthPrepassCutOutPipeline`: `cut_out_prepass.frag` (`cut_out.frag`'s work taking every output of `triangle.vert`, so validation does not warn), writing 0 to the solid mask; cut-outs drawn last. Contact shadows pass over those pixels.
- Same `drawBatches` as the main pass.

**Main pass** (`drawMainPass`):

- Scene colour `ColourTarget`, discarded and cleared to the sky background colour.
- After the prepass: depth kept and loaded, `EQUAL`, not written (`mainPassDepthState(true)`), so each pixel is shaded once whatever the draw order. Without it: cleared, `GREATER`, written.
- `EQUAL` needs bit-identical depths: both passes use `triangle.vert`, whose `gl_Position` is `invariant`.
- `mainPipelines` indexed by `mainPipelineIndex(afterPrepass, probes, surfaces, cutOut, glassRays)`: probes and cut-outs as fragment specialization constants (frames without them draw as plain); reflection surfaces add three targets (`triangle_surfaces.frag`); glass rays use `triangle_rays.frag` / `triangle_surfaces_rays.frag` in a layout with the ray tracing set. After the prepass a cut-out needs no discard (its holes have no depth to match), so no cut-out variant exists there.
- Shading itself is `scene_surface.glsl` (`triangle.frag` only includes it); a light is skipped before its shadow lookups when `facesLight` says the surface faces away (same test as the shading, so no change to the picture).
- With reflection surfaces the sky and stars need a block of their own (they write the scene image alone); depth stored only when read later.
- Ends with the scene image in `SurfaceRead`.

## Frames in flight

- `EngineSettings::framesInFlight`, 1 to `maxFramesInFlight` (2), clamped by `validFramesInFlight`.
- Each frame owns: command pool and buffer, fence (created signalled), acquire semaphore, `FrameDescriptors`, `FrameUniforms`, `SceneBuffers`, timestamp query pool.
- Shared by all frames: the frame images and the cluster masks (the fence order keeps them safe).
- `setFramesInFlight`: waits for the GPU, `createFrames`, tells every per-frame helper (debug lines, depth ranges, reflections, eye adaptation, ray tracing, AO, contact shadows, blur, depth of field), drops cloth and probe read-backs a vanished frame would never collect; the swapchain is rebuilt because `swapchainFramesInFlight` differs.
- ImGui keeps `maxFramesInFlight` buffer sets throughout.
- Swapchain images: at least one per frame in flight, the surface's minimum, three for Mailbox (`chooseSwapchainImageCount`; format, present mode and extent choices beside it, tested in `swapchain_choice_test`).

## Capture

`capture`: the frame drawn at the golden size into a capture image (`R8G8B8A8_SRGB`, or `R16G16B16A16_SFLOAT` for HDR) with its own tone mapping pipeline; TAA and AO histories restarted, warm-up frames first (probe warm-up, one for the depth fit, TAA's settling); scene and final images copied into one read-back buffer, waited for, read; the display output restored.

## Pipelines and the loading screen

- **`PipelineCompiler`** (owned by `VulkanDevice`, stopped first in its teardown): one worker per core (`defaultWorkers`). `submit` → `PipelineFuture`; `take` waits and hands it over or rethrows; `progress`; `waitAll` rethrows the first unreported error. A future dropped untaken destroys its pipeline; a build whose future is gone before it starts is skipped. Holds nothing of the GPU, so `pipeline_compiler_test` uses stand-in handles.
- **`VulkanPipeline`, `ComputePipeline`:** make their layouts at once (`DescriptorLayouts` is not thread-safe), hand the SPIR-V load and `vkCreate*Pipelines` to the compiler as a static build over copies of what it needs; the first `get()` waits for its own build.
- **Loading screen:** `Application` runs `onInit` while pipelines compile, then `waitForPipelines` calls `drawLoadingScreen` about 30 times a second, handling events (closing quits). "BUILDING SHADERS", built / total and a bar (`layoutLoadingScreen`), drawn by `vkCmdClearAttachments` alone in a 5 x 7 block font (`blockText`), since no pipeline may be ready.
- Not compiled this way: ImGui's own pipeline, the GPU tests' (built on first use).

## Kept from SDL_GPU

The port drew every golden image bit for bit the same, so these stay:

- **Descriptor layout** for bloom, tone mapping, debug lines and the overlay (`ShaderResources`, sets shared through `DescriptorLayouts`): set 0 vertex textures (unused), set 1 vertex uniforms, set 2 fragment combined image samplers then storage buffers, set 3 fragment uniforms. Scene pipelines use the GPU scene's layout instead (`PipelineDescription::sharedLayout`).
- **Pipelines:** depth clamp on, counter-clockwise front faces, line width 1, dynamic viewport and scissor. `setViewport` flips y (origin at `y + height`, negative height); `beginRendering` sets the whole target.
- **Formats:** swapchain `B8G8R8A8_SRGB` (else `R8G8B8A8_SRGB`), or `R16G16B16A16_SFLOAT` in `EXTENDED_SRGB_LINEAR` for HDR; bloom levels `B10G11R11_UFLOAT_PACK32` when drawable, blendable and filterable.
- **Shared samplers:** surfaces (trilinear, repeat, anisotropy 16 or the GPU's limit, `VK_LOD_CLAMP_NONE`); tables (bilinear, clamped); nearest; linear clamped; shadow compare (`GREATER_OR_EQUAL`, linear); shadow raw depth (nearest).
- **Mipmaps:** linear blits from the level above, each `max(1, size >> level)`; a cut-out's built on the CPU to keep its coverage (`coveragePreservingMips`) and copied in with level 0; a BC7 texture's every level built on the CPU and copied in (the GPU cannot write blocks: [assets.md](assets.md#texture-compression)).

## Uniform blocks (`FrameUniforms`)

SDL's push semantics. The 4 KB limit and why: `src/render/CLAUDE.md`.

- `push(stage, slot, data)` copies into the frame's 4 MB upload buffer at the next aligned offset (`UniformRing`: each slice leaves room for a whole `maxUniformBlockBytes` range).
- `bind` binds sets 1 and 3 (`UNIFORM_BUFFER_DYNAMIC`, 4 KB ranges) with the slots' latest offsets. A full buffer moves on to the next (kept for reuse), copying the latest blocks along, so a draw's blocks share one buffer. `flush()` before submit.
- Set 2 written per draw (`bindFragmentResources`) from `FrameDescriptors`: pools of 1024 sets (8192 samplers, 1024 storage buffers, 16 uniform buffers, 16 storage images), reset each frame; a pool added when one runs out.

## Synchronisation (`image_state`)

An `ImageState` names a use; `imageStateUse` gives its stages, accesses and layout.

| State | Use |
|---|---|
| `Undefined`, `Acquired`, `Present` | Start; a swapchain image just acquired (stage = `acquireWaitStage`, so the first barrier chains after the semaphore); presented (no stage: the semaphore orders it) |
| `ColourTarget`, `DepthTarget` | Drawn into |
| `FragmentRead` | Sampled by fragment shaders |
| `SurfaceRead` | Sampled by fragment and compute (every uploaded texture, since probe rays read them) |
| `VertexRead` | Sampled by vertex and compute (the wind's springs) |
| `DepthFragmentRead`, `DepthComputeRead` | A depth image sampled |
| `ComputeRead`, `ComputeStorage` | Sampled by compute; read and written in place (`GENERAL`) |
| `TransferSource`, `TransferDestination` | Copies and blits |

- `imageBarrier(image, aspect, from, to, ImageContents, levels)` waits for the old stages, flushes only the old writes, makes data visible to all new accesses, changes layout (from undefined on `Discard`). `needsBarrier` is false only for the same read-only state twice.
- `VulkanImage::transition` / `transitionImage` record it and remember the state across frames, so a write after last frame's read still waits.
- Buffers: `bufferBarrier(commands, buffer, before, after)` with `BufferUse{stages, access}`.
- Tested in `image_state_test`.

## Device and memory

- **`VulkanDevice`:** `volkInitialize` once per process (vk-bootstrap caches the loader's first functions; SDL unloads its copy when video stops); without a window (GPU tests) vk-bootstrap's headless mode, no surface. Vulkan 1.3, discrete GPU preferred, one queue for everything, VMA.
- **Required features:** `samplerAnisotropy`, `depthClamp`, `shaderClipDistance` (planar mirrors), `shaderStorageImageExtendedFormats`; 1.3's `dynamicRendering`, `synchronization2`, `shaderDemoteToHelperInvocation`, `subgroupSizeControl`, `computeFullSubgroups`, `privateData`; descriptor indexing for bindless textures and `bufferDeviceAddress`. `checkFormats` throws without fragment and compute subgroup arithmetic (clustered shading, the cascades' fit).
- **Asked first, dropped only if no GPU has them:** FSR's and DLSS's (`shaderInt16`, `shaderFloat16`, storage images without format, `timelineSemaphore`, `descriptorIndexing`, plus `VK_KHR_get_memory_requirements2` and `VK_KHR_dedicated_allocation`, since FSR fetches the KHR-suffixed name).
- **Optional:** DLSS's extensions (all or none), memory budget, dual-source blending, independent blending, ray queries (acceleration structures, ray query, deferred host operations).
- **Debug utils:** on whenever the instance has it, release too. `beginLabel`/`endLabel` mark passes; `nameObject` names objects (`ImageDescription::name`, a string literal per image: Scene colour, Scene depth, Solid mask, Shadow atlas, Still shadow, Golden capture, ...).
- **Memory:** `VulkanBuffer` in `BufferMemory::Gpu`, `Upload` (mapped, sequential writes) or `Readback` (mapped, cached); `VulkanImage` (2D, cube or volume, a view of all levels, its current `ImageState`). `ImmediateCommands::run` records, submits and waits (uploads, atlas clear, GPU tests). `OwnedHandle<T>` owns one device object; `vulkanCheck` turns a failed `VkResult` into an exception naming it.

## Debug lines and overlay

Drawn after tone mapping in the swapchain's block, with the world-space `viewProjection`: debug lines (`VulkanDebugLines`, `game-interface.md`), then the UI overlay (`VulkanOverlay`, `settings-and-commands.md`).

## Software renderer

`render/software/`: a reference rasterizer used only by tests. `Renderer`, `Display`, `PixelBuffer`, `DepthBuffer`, `rasterizer`, `clipper`, `shading`. Flat shading only; ignores per-vertex normals and UVs.
