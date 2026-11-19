# Golden images and GPU tests

Covers: `src/engine/golden`, `src/engine/golden_capture`, `src/engine/golden_view.h`, `src/render/captured_frame.h`, `src/core/image_compare`, `src/core/float_tolerance`, `tests/gpu/`, `tests/golden/`

The GPU's two test layers: whole pictures kept as references, and every shader library function run on the GPU against its C++ mirror. Steps: `.claude/skills/golden/SKILL.md` (running, saving), `.claude/skills/add-shader-function/SKILL.md` (a new function or library). Rules: `.claude/rules/shader-mirror.md`. The CPU suite: `tests/CLAUDE.md`.

## Golden images

Fixed views of each game, rendered and compared pixel by pixel, so a renderer change that alters the picture is seen.

### Views

- `GoldenView` (`engine/golden_view.h`): `name` (also the file name; `isGoldenViewName`: lower-case letters, digits, dashes), `simulationSeconds`, `hdr`.
- A game lists them in `Game::goldenViews()` and sets one up in a world fresh from `onInit` with `Game::prepareGoldenView` (camera pose through `PlayerControls::poseFreeCamera`, switches, the sun's hour with its clock stopped so the ticks do not move it).
- Each game keeps its table beside its constructor:

| Game | Table | Views |
|---|---|---|
| `demo` | `demoGoldenViews` | `start`, `start-hdr`, `gallery`, `lamp-field`, `floodlights`, `moon` |
| `lightlab` | `labGoldenViews` | `towers`, `overview`, `sunlit-room`, `sunset-hdr`, `sun-disc` (facing the sun at its hour), `flashlight` (the back room by a fast light), `back-room-lamp`, `moonlit` (day 88, 21:00, facing the moon), `street-night` (22:00) |
| `clothlab` | `clothLabGoldenViews` | `stations` |
| `forest` | `forestGoldenViews` | `rim`, `ring`, `wall`, `rim-dusk` |

- `advanceToGoldenView(game, world, view, ticksPerSecond)`: prepare, `round(seconds × rate)` ticks through `runFixedUpdate`, then one `onUpdate` with an empty `Input`. Shared by the capture and `golden_views_test`: `testGoldenViews` (routine) checks every game's view names; `testGoldenViewScenes` (slow tests, run before a golden run) that each pose holds without input and two fresh builds reach the same `hashSimulationState` and camera.

### Capturing

`main` with `--golden-save` or `--golden-check` calls `runGoldenImages(makeGame, gameFolder, mode)` for every registered game, or the one named by `--game`; the folder is the registry name (`make golden`, `make golden-save`). Per view:

1. A fresh game and `Application` (640x360 window, `EngineSettings{}` with `commandServer` off, `SavedSettings::Ignore`), so no view sees another's switches, `settings.cfg` changes nothing, and a running game keeps its port.
2. `Application::captureGoldenView`: `advanceToGoldenView`, upload new assets, `describeFrame(1, tick)`, then `VulkanRenderer::capture` at `goldenImageWidth` x `goldenImageHeight` (1280x720) with `goldenDisplayOutput(hdr)`.
3. `capture`, before its frame:
   - starts every history afresh (TAA, ambient occlusion, FSR, DLSS, the previous camera, the jitter at frame 0) and bakes the whole sky visibility grid at once;
   - draws the frame first `framesBefore` times (`drawFramesBeforeCapture`), the largest of: `goldenProbeWarmUpFrames` (960, 8 s of ticks) when light probes are traced, else 1 with `fitCascadesToDepth` (the cascades need a measured depth range); `taaSettleFrames` (two jitter cycles, 32) with any anti-aliasing on;
   - the repeats drop the frame's probe events (they happen once); afterwards the light history, contact light choice, atlas layout and shadow cache are put back, so the capture is otherwise a first frame;
   - `capturing` snaps eye adaptation to its target on every frame drawn.
4. The captured frame: `drawScene` (shared with `render`), then tone mapping into an off-screen target in `captureFormat` (`R8G8B8A8_SRGB` as the SDR swapchain, or `R16G16B16A16_SFLOAT` scRGB) through a tone mapping pipeline built for it (`createToneMapPipeline`). Both images are copied to a read-back buffer; the GPU is waited for. No debug lines or UI; nothing presented; the display output restored.
5. `CapturedFrame` (`render/captured_frame.h`): `scene` (half floats: the finished picture, `finishedScene`, after anti-aliasing and before depth of field and motion blur) and `final` (what the screen gets).

- `goldenDisplayOutput`: SDR, or HDR on a fixed screen (SDR white at scRGB 2.5, 200 nits; headroom 4), so captures do not depend on the monitor or Windows.
- Defaults apply: ray tracing is off, so no default view traces probes; the default upscaler falls back to TAA where the DLSS or FSR DLLs are missing.

### Files

- `tests/golden/<GPU>/<game>/<view>-scene.png` and `<view>-final.png`; `<GPU>` is `goldenFolderName(device name)` (letters and digits kept, other runs a dash). Layout notes: `tests/golden/README.md`.
- Half-float images are stored losslessly in 8-bit PNGs twice as wide (`packHalfImage`, `unpackHalfImage` in `core/raw_image`; `saveRawImagePng`, `loadRawImagePng` in `assets/image_loader`).
- A check writes its captures, and a `-diff.png` heatmap for any image with a difference (black equal, yellow within tolerance, red beyond), to `build/golden/<GPU>/<game>/`, prints a line per image and exits 1 on any failure. A missing reference counts as a failure, so a game with views but no saved references on this GPU fails `make golden`.

### Comparing

- `compareImages(reference, captured, ImageTolerance)` (`core/image_compare`) counts differences in steps: bytes, or half-float steps (`halfStepDistance`, `core/half_float`: +0 and -0 equal, NaN never close).
- `ImageTolerance{}` passes when shapes match, no pixel is more than 1 step off, and at most 0.1% of pixels differ at all (a whole image one step brighter is a real change).
- References hold only for the GPU, driver and shaders that made them; there, repeated runs and both builds match bit for bit.
- CPU tests of the machinery: `image_compare_test`, `half_float_test`, `raw_image_png_test`, `golden_views_test`.

## Shader libraries and their mirrors

Pure-function libraries in `assets/shaders/*.glsl`, included by the passes and by the test shaders; each has a C++ mirror with its own CPU tests.

- An include guard, and the `*_shared.h` headers the library needs (numbers both sides use).
- Everything comes in as arguments: no texture reads, `gl_FragCoord` or derivatives, which compute shaders lack. The passes read textures and pass the values in (`triangle.frag` the BRDF table, `tone_map.frag` the noise).
- Only `.vert`, `.frag` and `.comp` compile; `.glsl` files are only included.

| Library | C++ mirror | GPU test (`tests/gpu/`) |
|---|---|---|
| `brdf.glsl` | `render/gpu/brdf` | `brdf_gpu_test.cpp` |
| `normal_mapping.glsl` | `render/gpu/normal_mapping` | `normal_mapping_gpu_test.cpp` |
| `parallax.glsl` | `render/gpu/parallax` | `parallax_gpu_test.cpp` |
| `ground_layers.glsl` | `render/gpu/ground_layers` | `ground_layers_gpu_test.cpp` |
| `lighting.glsl` | `render/gpu/light_falloff`, `render/gpu/shadow_map` | `lighting_gpu_test.cpp` |
| `soft_shadow.glsl` | `render/gpu/soft_shadow` | `soft_shadow_gpu_test.cpp` |
| `tone_mapping.glsl` | `render/gpu/tone_mapping` | `tone_mapping_gpu_test.cpp` |
| `bloom.glsl` | `render/gpu/bloom` | `bloom_gpu_test.cpp` |
| `eye_adaptation.glsl` | `render/gpu/eye_adaptation` | `eye_adaptation_gpu_test.cpp` (and its passes) |
| `taa.glsl` | `render/gpu/taa` | `taa_gpu_test.cpp` |
| `screen_reflections.glsl` | `render/gpu/screen_reflections` | `screen_reflections_gpu_test.cpp` |
| `reflection_probe.glsl`, `planar_reflection.glsl` | `render/gpu/reflection_probe`, `render/gpu/planar_reflection` | `reflection_probe_gpu_test.cpp` |
| `mirrored_light.glsl` | `render/gpu/mirrored_light` | `mirrored_light_gpu_test.cpp` |
| `ambient_occlusion.glsl` | `render/gpu/ambient_occlusion` | `ambient_occlusion_gpu_test.cpp` |
| `contact_shadows.glsl` | `render/gpu/contact_shadows` | `contact_shadows_gpu_test.cpp` |
| `volumetric_fog.glsl` | `render/gpu/volumetric_fog` | `volumetric_fog_gpu_test.cpp` |
| `alpha_mask.glsl` | `render/gpu/alpha_mask` | `alpha_mask_gpu_test.cpp` |
| `transparency.glsl` | `render/gpu/transparency` | `transparency_gpu_test.cpp` |
| `sky_visibility.glsl` | `render/gpu/sky_visibility` | `sky_visibility_gpu_test.cpp` |
| `camera_finish.glsl` | `render/gpu/camera_finish` | `camera_finish_gpu_test.cpp` |
| `motion_blur.glsl` | `render/gpu/motion_blur` | `motion_blur_gpu_test.cpp` |
| `depth_of_field.glsl` | `render/gpu/depth_of_field` | `depth_of_field_gpu_test.cpp` |
| `dither.glsl` | `core/srgb`, `core/dither` | `dither_gpu_test.cpp` |
| `window_mask.glsl` | `render/gpu/window_mask` | `window_mask_gpu_test.cpp` |
| `culling.glsl`, `level_of_detail.glsl` | `render/gpu/cull_records` | `cull_records_gpu_test.cpp` |
| `camera_culling.glsl` | `render/gpu/camera_culling` | `cull_records_gpu_test.cpp` |
| `level_fade.glsl` | `render/gpu/level_fade` | `level_fade_gpu_test.cpp` |
| `overlay.glsl` | `render/gpu/overlay_composite` | `overlay_gpu_test.cpp` |
| `clusters.glsl` | `render/gpu/clusters` | `clusters_gpu_test.cpp` |
| `cascades.glsl` | `render/gpu/cascades` | `cascades_gpu_test.cpp` |
| `depth_range.glsl` | `render/gpu/depth_range` | `depth_range_gpu_test.cpp` (the real pass only) |
| `stars.glsl` | `render/gpu/stars` | `stars_gpu_test.cpp` |
| `atmosphere.glsl` | `render/gpu/atmosphere` | `atmosphere_gpu_test.cpp` (and its passes) |
| `ray_tracing.glsl` | `render/gpu/ray_tracing` | `ray_tracing_gpu_test.cpp` (and ray queries) |
| `ddgi.glsl` | `render/gpu/ddgi` | `ddgi_gpu_test.cpp` (and the probe passes) |
| `ddgi_sampling.glsl` | `render/gpu/ddgi_sampling` | `ddgi_sampling_gpu_test.cpp` |
| `probe_relocation.glsl` | `render/gpu/probe_relocation` | `probe_relocation_gpu_test.cpp` |
| `terrain.glsl` | `scene/terrain_heights` | `terrain_gpu_test.cpp` |
| `impostor.glsl` | `scene/impostor` | `impostor_gpu_test.cpp` |
| `card_thinning.glsl` | `scene/card_thinning` | `card_thinning_gpu_test.cpp` |
| `facet_variation.glsl` | `render/gpu/facet_variation` | `facet_variation_gpu_test.cpp` |
| `wind.glsl` | `render/gpu/wind` | `wind_gpu_test.cpp` |
| `cloth.glsl`, `deform.glsl` | `src/cloth/` itself, `scene/deforming_mesh` | `cloth_gpu_test.cpp` (and the cloth passes) |

- Without a test shader of their own: `probe_cascades.glsl` (`render/gpu/probe_cascades`; checked through the real cascade passes in `ddgi_gpu_test.cpp`), `ray_scene.glsl` (through `ray_query_test.comp`), and helpers such as `hash.glsl`, `fibonacci.glsl`, `impostor_sampling.glsl` reached through the libraries that include them.

## The GPU test program

`build/gputests.exe`, its own program (`make gputest`, `make gputest-release`), so `tests.exe` needs no GPU:

- built from `tests/gpu/*.cpp` and the engine objects; `tests/gpu/shaders/*.comp` compiled with `-Isrc -Iassets/shaders -Itests/gpu` (dependency files in `build/obj/shaders/gpu_tests/`);
- the make target then runs `game.exe --golden-check`.

`gpu_test_main`: a windowless `VulkanDevice{nullptr, true}` (validation on), one `GpuFunctionRunner`, then every `gpuTest<Library>` in order, the report printed. Exit 1 on any failed group, any validation error, or an exception.

### The runner

`GpuFunctionRunner::run(shader, function, inputs, inputsPerCase, outputsPerCase)` (`tests/gpu/gpu_function_runner`):

- Inputs as `GpuVec4` (`std::array<float, 4>`, the same 16 bytes as a std430 `vec4`, so no padding to get wrong) in an upload storage buffer; outputs in a read-back storage buffer.
- One descriptor set from a one-set pool reset each run; push constants `{count, function, inputsPerCase, outputsPerCase}`.
- `vkCmdDispatch(workgroupCount(count, GPU_TEST_GROUP_SIZE))`, a compute-write to host-read barrier, `ImmediateCommands::run` (submit and wait), `invalidate`. Host writes need no barrier: submission makes flushed writes visible.
- One `ComputePipeline` per shader, made on first use and kept.

Test shaders:

- `tests/gpu/shaders/gpu_test.glsl`: the two buffers, the `TestCase` push constants, `local_size_x = GPU_TEST_GROUP_SIZE` (64: whole NVIDIA and AMD waves), `outsideTheCases`, `testInput(index)`, `testOutput(index, value)`. One invocation is one case.
- `<library>_test.comp`: branches on `function` and calls the library.
- `tests/gpu/gpu_test_functions.h` (macro-only, read by both sides): the group size and each shader's `GPU_TEST_<NAME>` numbers, counted from 0 per shader.

Helpers:

- `tests/gpu/gpu_check`: `CaseRandom` (xorshift32, the same inputs every run on every standard library; `between`, `direction` uniform on the sphere), `gpuVec4`, `vec3From`, `gpuTestShader(name)` (the compiled `.comp.spv` path), and `GpuCheckReport`.
- `GpuCheckReport`: `section` (a library), `check(name, tolerance, expected, actual)` for floats and `Vec3`s, grouped by name; `print` shows per group the count, pass or FAIL, and the worst comparison's share of its tolerance with both values, so a narrow margin shows before it fails; `failedGroups`.
- `tests/gpu/image_readback`: `readHalfImage` (with `ReadImage::texel` and a `bilinear` mirror of a linear clamped sampler), `readFloatBuffer`.
- `tests/gpu/cpu_trace`: `traceOnCpu`, the CPU's first hit over every triangle a ray's mask lets it see, the reference for ray queries.

### Tolerances

`core/float_tolerance`:

- `FloatTolerance{absolute, relative}` allows `absolute + relative * |expected|`.
- `toleranceUsed`: 0 when equal (equal infinities too), infinite for a NaN or any difference where none is allowed; `withinTolerance` is a share of at most 1.
- `ToleranceTally`: `checked`, `failures`, `worstUse` with `worstExpected` and `worstActual`; `add` with the tally's tolerance or one per comparison.

Chosen in `gpu_check.h`, sized by what Vulkan lets a GPU do differently (fused multiply-adds, division and sqrt a few ULP off), while real bugs are off by percent:

| Tolerance | Absolute, relative | For |
|---|---|---|
| `arithmeticTolerance` | 1e-6, 1e-5 | + - * / and sqrt |
| `transcendentalTolerance` | 1e-6, 1e-4 | pow, exp2, log2 |
| `trigonometricTolerance(amplitude)` | 2^-11 × amplitude, 1e-4 | results scaled by sin or cos, whose allowed error is absolute |
| `exactTolerance` | 0, 0 | whole numbers (cube faces, bins), copies |

Tolerances that follow the problem, each with a comment at its test:

- `interleavedGradientNoise`: compared round the circle (it wraps at 1) within 4e-3, since at 4K pixel coordinates its inner product keeps few digits.
- `addLight` specular: plus `4 * 4 epsilon / d` relative, d = 1 - nDotH^2 (1 - alpha^2), GGX's denominator, since near a sharp highlight's peak one float step in nDotH moves the result by about 4 delta / d.
- Half-float atlases: a whole stored step, since Vulkan lets a GPU round toward zero when storing 16-bit floats.

Left to the golden images: texture filtering (bloom's filters, PCSS's sampling, the BRDF table lookup).

### Tests of real passes

Beside the function tests, some run the renderer's own passes on inputs whose answer the CPU works out:

| Test | Checks |
|---|---|
| `gpuTestDepthRange` | `VulkanDepthRange` on uploaded depth images, bit for bit against the CPU range: random surfaces with sky, sizes cut mid-group, a lone surface in the last pixel, the near plane and the smallest normal float (denormals may flush to sky), sky only |
| `gpuTestAtmosphere` | `VulkanAtmosphere`'s tables and flat ambient against the CPU's marches |
| `gpuTestEyeAdaptation` | the histogram and adaptation passes against `adaptToHistogram` |
| `gpuTestRayTracing` | on GPUs with ray queries: a cube, a sphere and a plane placed five times (turned, stretched, mirrored, visual-only, a floor), 2,000 rays from in front and from inside the cube, solid-only or all, through `ray_query_test.comp`; hit, instance, triangle and facing exact, distance and normal within a tolerance growing as 1/cosine (a grazing hit slides along the ray) |
| `gpuTestDdgi` | on GPUs with ray queries, `VulkanDdgi`'s trace, update, sharing, relocation and seed passes, atlases read back as half floats, every border texel an exact copy: the furnace (sky everywhere, longest distance), a probe sealed in a box, a sunlit floor, two lamps (one kept per hit; fast and still atlases), sharing, more rays than one update chunk, a wall between two probes, relocation against `relocateProbe`, and two cascades moved a metre (the plane brought in seeded from the coarser one; with states on, off its first frame, then vigilant by a static floor) |
| `gpuTestCloth` | `VulkanCloth` ticks from the CPU's state against `stepCloth` (whole runs diverge chaotically, so tick by tick), repeat runs bit for bit, a settled sheet's yardstick measures, and the shape pass against `deformVertices` |

A scene for the probe tests is written as the renderer writes it (`BindlessTextures` with white at slot 0, `SceneLayout`, `SceneBuffers`, stand-in shadow atlas and BRDF table, the probe passes' own fresh atlases as set 1's) with a speck far below, since a top-level structure needs something in it.

## Compute pipelines

The runner's pipelines are the renderer's: `ComputePipeline` (`render/vulkan/vulkan_compute`) from a `ComputePipelineDescription`; header for its fields.

- Its own set: storage buffers from binding 0, then combined image samplers, then storage images; a compute push constant range.
- `sharedLayout` (not owned) takes a layout made elsewhere, the scene's for passes reading the frame's set; `sharedSets` puts sets made elsewhere before its own, which then sits at `getResourceSet()`.
- `workgroupCount(count, groupSize)` (`render/gpu/compute_dispatch`, `compute_dispatch_test`): groups covering `count`, rounded up without overflow.
- Users: the GPU tests and about twenty renderer passes (`render/vulkan/vulkan_*.cpp`).
