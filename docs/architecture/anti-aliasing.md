# Anti-aliasing and upscaling

Covers: `src/render/gpu/taa`, `src/render/gpu/upscaling`, `src/render/vulkan/vulkan_taa`, `src/render/vulkan/vulkan_fsr`, `src/render/vulkan/vulkan_dlss`, `assets/shaders/taa.glsl`, `assets/shaders/taa.comp`, `assets/shaders/motion.vert`, `assets/shaders/motion.frag`, `assets/shaders/motion_fill.comp`

Each pixel averages what it saw over many frames while the projection shifts by a different sub-pixel offset each frame, so edges and thin things get many samples for the price of one. The same machinery upscales: the scene may be drawn smaller than the output. Shader maths rules: `.claude/rules/shader-mirror.md`.

## The one choice

`UpscalingSettings::upscaler` (`RenderSettings::upscaling`, F5 Upscaling, "Anti-aliasing"):

| Value | What builds the finished picture |
|---|---|
| `Off` | nothing: no jitter, no velocity pass, no history; the scene image is the finished picture |
| `Taa` | the engine's own resolve (`taa.comp`) |
| `Fsr` | AMD FSR 3.1 (`VulkanFsr`), settings-file name "FSR 3.1" |
| `Dlss` (default) | NVIDIA DLSS through Streamline (`VulkanDlss`); at render scale 1 this is DLAA |

- `temporalAntiAliasing(settings)`: anything but Off. FSR and DLSS replace only the resolve; they still use the jitter and the velocity pass.
- `activeUpscaler(settings, UpscalerSupport)`: FSR or DLSS only where their library loaded and the GPU can run it, else TAA. A failed dispatch also makes that upscaler unavailable and the frame falls back to the resolve.
- Each path drops the others' history (`taa->forgetHistory()`, `fsrHistory`, `dlssHistory`), so switching starts afresh.
- The F4 exposure line ends with the upscaler in use and the scale (`upscalerText`, e.g. `FSR 3.1 AT 0.67`).
- All three are timed as one pass, `GpuPass::AntiAliasing` (readout AA), after the glass and before depth of field, motion blur and eye adaptation.

## Settings

`TaaSettings` (`RenderSettings::taa`, `render/gpu/taa.h`) tunes only the engine's resolve: `currentWeight` (0.1, about ten frames), `clipSigma` (1), `jitter`, `showMotion`. `UpscalingSettings` (`render/gpu/upscaling.h`): `upscaler`, `renderScale`, `sharpness` (FSR's RCAS, 0 off), `dlssModel` (M; `dlssPresetNumber` maps to Streamline's presets). Meanings and limits: the headers.

## Jitter

- `taaJitter(frame, phases)`: the Halton (2, 3) point of the frame's place in the cycle, counted from 1, minus a half: within half a render pixel of the centre. `halton` is the radical inverse.
- Cycle: `TAA_JITTER_FRAMES` (16) for the engine's resolve at any scale; `jitterPhaseCount(render, outputWidth)` = ceil(8 n²) (n = output over render width) for FSR and DLSS, as AMD's table.
- `jitteredClip(viewProjection, offset, width, height)` shifts clip x and y by offset × w (y negated, pixels count down), so every point moves the same and depth is unchanged.
- Jittered: the camera-relative view-projection of the depth prepass, main pass and probe balls, at the render size. Unjittered: culling and debug lines, shadow views, sky rays, stars, and `motionViewProjection` (the camera kept for motion and reprojection).

## Motion

**Still things and the sky** reproject from depth:

- `previousRelativeViewProjection(previous, previousOrigin, currentOrigin)`: last frame's camera-relative matrix taking points measured from this frame's camera (small numbers).
- `taaReprojection(current, previous)` = previous × `inverse(current)` (general Gauss-Jordan inverse in doubles): exactly identity for a still camera, so a still picture reads its own texels.
- `reprojectPixel`: pixel and depth through it; valid only with w > 0. Depth 0 (the far plane) reprojects by the camera's turn alone.

**Moving objects** get motion vectors:

- `DrawItem::previousModel` comes from `MotionHistory` (`scene/frame_description`, owned by `Application`, render-only): records by entity slot with a frame stamp in two alternating sets, so nothing is cleared or allocated per frame. None on an entity's first drawn frame.
- `drawScene` batches the visible draws whose `previousModel` differs from `model` after the shadow tiles' entries in the draw list; set 1 binding 21 (`PreviousModelsBinding`) holds each instance's previous model (its current one when unknown).
- Velocity pass (`drawMotion`, `GpuPass::Motion`, MOTION): an `R16G16_SFLOAT` image at the render size cleared to `TAA_NO_MOTION` (1000); `motion.vert`/`motion.frag` draw the movers over the main pass's depth (test `GREATER_OR_EQUAL`, no write; back faces culled except double-sided batches; `motionCutOutPipeline` discards cut-out holes). Output `objectMotion`: this frame's unjittered NDC minus last frame's, none when either w ≤ 0. Push constants `GpuMotionView` (128 bytes).
- `previousPixelFromMotion` turns a motion back into last frame's pixel; a value above `TAA_NO_MOTION_TEST` means "use the camera's".
- Camera motion pass (`motion_fill.comp`, `VulkanTaa::fillCameraMotion`, `GpuMotionFillPass`), before FSR or DLSS only: each pixel still at no-motion gets `cameraMotion(reprojection, pixel, depth, size)`, since they need a motion everywhere. The engine's resolve reprojects such pixels itself.

## The resolve

`taa.comp`, one invocation per output pixel in `TAA_GROUP_SIDE` (8) squares, push constants `GpuTaaPass` (112 bytes):

1. **Current colour rebuilt** from the 3 × 3 render pixels round `renderPixelOf(pixel)` (render pixel t shows the unjittered picture at t + 0.5 − jitter). Each weighs `taaSampleWeight(d²)` = exp(−`TAA_SAMPLE_FALLOFF` d²) (2.29, Karis's Gaussian fit of Blackman-Harris), d its sample's distance in output pixels (`sampleOutputPixel`).
2. The same 9 pixels in YCoCg (`toYCoCg`): mean and standard deviation.
3. **History place:** at the nearest of the 9 to the camera (depth dilation, largest reversed-Z depth, so moving edges carry their motion): from the velocity image if it holds a motion, else reprojected from that depth. Off screen, behind last camera or with no valid history: this frame alone.
4. History read with Catmull-Rom (`catmullRomWeights`, nine bilinear reads), clamped positive, so repeated reads do not blur.
5. Clipped towards the box centre into mean ± `clipSigma` deviations (`clipToBox`, Playdead's clip).
6. Blended by `taaBlend`, each side times its Karis weight 1 / (1 + luminance), so one bright sample cannot flash. Share: `taaCurrentShare` = setting × nearest sample's weight, so a pixel no sample landed near keeps its history. Then `taaReactiveShare(share, reactive)` = share + (1 − share) × reactive.
7. `showMotion`: motion in pixels × `TAA_MOTION_VIEW_SCALE` as red and green (blue: no history).

**Images** (`VulkanTaa`): the velocity image at the render size; two `R16G16B16A16_SFLOAT` histories at the output size taking turns. This frame's history is `getResult()`, the renderer's `finishedScene`, read by everything after (eye adaptation, bloom, tone mapping, captures).

**Barriers:** velocity from colour target to compute read; reactive mask already compute read; depth to compute read; last history from `SurfaceRead` (or a capture's copy source) to compute read; this frame's from discarded to storage, then `SurfaceRead`.

**History restarts:** first frame; output size change (`ensureSize`; a new render size remakes only the velocity image); anti-aliasing off (last camera forgotten too); every capture (also resets `taaFrame`, restarting the jitter).

## The reactive mask, as read here

Glass and blended surfaces change a pixel in ways motion vectors cannot follow. `VulkanReactiveMask` (`render/vulkan/vulkan_reactive_mask`, built with the glass; `reactiveShare` in `render/gpu/transparency.h`) gives an R16F mask at the render size, cleared to 0 in frames without glass. Readers:

- the resolve: `taaReactiveShare`;
- FSR: its `reactive` input;
- DLSS: tagged `kBufferTypeBiasCurrentColorHint`.

## Upscaling

- `renderScale` (1; `minRenderScale` 0.33 to 1) applies only while anti-aliasing is on (`appliedRenderScale`), since it is what upscales.
- `renderSize(width, height, scale)`: each side rounded, 1 to the output's. `ensureFrameTextures` keeps the scene image, depth, velocity image and cluster lists at it (`sceneSize`) and remakes them on change.
- At the render size: prepass, main pass, sky, stars, probe balls, ray debug view, velocity pass, clusters, depth measurement, ambient occlusion. At the output's: histories, everything after anti-aliasing, debug lines and UI. Shadows, probes and sky tables are screen-independent.
- **Modes** (`UpscalingMode`: Native, Quality, Balanced, Performance, Ultra performance = scales 1, 1/1.5, 1/1.7, 1/2, 1/3, `upscalingModeScale`): not stored. F5's combo sets the scale; `matchingUpscalingMode` reads it back within 0.005, else Custom. Presets leave the scale alone.
- **Mip bias** `textureLodBias(render, outputWidth, upscaler)` into `GpuFrameData::textureOptions.x`: log2(render / output width) for TAA; one level more (log2(scale) − 1) for FSR and DLSS, the vendors' advice. Mips are thus chosen for output pixels, and the history averages the extra detail.

## FSR

`VulkanFsr` loads `amd_fidelityfx_vk.dll` with `SDL_LoadObject` and takes the FidelityFX API's C functions by name (`ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`). A C interface is why an MSVC-built DLL works from GCC. SDK v1.1.4 is the last with a Vulkan backend. Missing DLL, missing MSVC runtime or missing device features: unavailable, the game carries on.

- **Context:** made at the first FSR frame with `maxRenderSize` = `maxUpscaleSize` = the output (a new scale needs no new context); flags `HIGH_DYNAMIC_RANGE`, `DEPTH_INVERTED`, `AUTO_EXPOSURE` (the engine's exposure is not passed), plus `DEBUG_CHECKING` in debug builds (messages to the log). `ensureSize` (GPU idle) remakes the `R16G16B16A16_SFLOAT` output and drops the context on an output-size change.
- **Each frame:** camera motion pass, then `dispatch` with `makeFsrFrameInputs`: jitter in render pixels (same convention as `jitteredClip`), `motionVectorScale` = (−w/2, +h/2) of the render size, frame time in ms, sharpness, near and far swapped (FSR names them by depth under reversed-Z), vertical field of view, `reset` when FSR did not draw last frame or the last camera is unknown.
- Images told to FSR in the states it returns them to: colour, depth, velocity, reactive mask in `ComputeRead`; output in `ComputeStorage`, then moved to `SurfaceRead` as `finishedScene`.

## DLSS

NVIDIA's own SDK (NGX) ships as MSVC static libraries GCC cannot link; Streamline is a DLL with `extern "C"` functions and plain structs, so it loads like FSR.

**Files:** Streamline's MIT headers are committed (`external/streamline/include`); its DLLs (`sl.interposer`, `sl.common`, `sl.dlss`, `NvLowLatencyVk`, `nvngx_dlss`) are fetched by `tools/fetch_streamline.py` (pinned, SHA-256 checked) into `external/streamline/bin`, copied beside each exe by the makefile, packaged by `tools/package_demo.py` with their licences and a DLSS credit.

**`StreamlineLibrary`** (a `VulkanRenderer` member declared before `VulkanDevice`: Streamline must start before the device):

- `sl.interposer.dll` beside the exe, signature checked by `sl::security::verifyEmbeddedSignature` (WinTrust), loaded once a run and never unloaded (golden runs make one renderer per game).
- `slInit` with `eUseManualHooking` (the engine makes and calls Vulkan itself), `eUseFrameBasedResourceTagging`, `eDisableCLStateTracking`; plugins from the exe's folder, no over-the-air updates; info messages and two expected warning kinds filtered.
- DLSS's feature requirements give the extensions (`getExtensions`) for `VulkanDevice`, minus `VK_EXT_buffer_device_address` (Vulkan forbids it beside the KHR one).
- `shutdown()` from the renderer's destructor, GPU idle, before the device goes.

**`VulkanDlss`** (after the device):

- `slSetVulkanInfo` (device, instance, GPU, queues) and `slIsFeatureSupported`. The swapchain presents through Streamline's `vkQueuePresentKHR` (`presentFunction`, `VulkanSwapchain::setPresentFunction`): manual hooking still needs every present for its bookkeeping.
- `renderSize(output, scale)`: DLSS works in modes (`nearestUpscalingMode`; Native is DLAA), each accepting a range of render sizes (asked once per output size and mode, cached), so the scale's size is clamped into it (`clampRenderSize`).
- `dispatch` after the camera motion pass: a frame token (`slGetNewFrameToken`, from `taaFrame`); constants from `makeDlssFrameInputs` (matrices in Streamline's row-vector layout, `streamlineMatrix`: the GPU lens and inverse, `taaReprojection` and inverse as clip-to-previous-clip; jitter in render pixels; `mvecScale` (−0.5, +0.5); camera position, axes, planes, field of view, aspect; depth inverted, camera motion included, `reset` when DLSS did not draw last frame); options (mode, output size, HDR, DLSS's own exposure, the model); tags `eValidUntilPresent` (colour, depth, velocity, reactive mask in `SHADER_READ_ONLY_OPTIMAL`, output in `GENERAL`), so Streamline reads in place; then `slEvaluateFeature`. DLSS records its own barriers; the output goes to `SurfaceRead` as `finishedScene`.
- DLSS's jitter convention is the motion vectors': render pixels, x right, y down, as `jitteredClip`'s. NVIDIA's development DLL has hotkeys that flip it, should a still scene ever shimmer.

## Captures

A capture draws `taaSettleFrames` (32, two cycles) first while anti-aliasing is on (more if probes or the cascades' depth fit want it), with every history, the last camera and `taaFrame` reset, so it repeats exactly. Golden captures use whatever upscaler the settings pick (DLSS by default, TAA where it is missing).

## Tests

`taa_test`, `upscaling_test` (CPU mirrors, packing, modes, sizes, vendor inputs); `taa_gpu_test` (the GLSL library against the mirror). Golden procedure: the `golden` skill.
