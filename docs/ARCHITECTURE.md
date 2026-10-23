# Architecture reference

The detailed reference for how each system is built today: names, files, numbers and the reasons behind them. `CLAUDE.md` holds the rules and a short summary and points here; `docs/ENGINE_GUIDE.md` explains the concepts for a human reader; `docs/BUILDLOG.md` keeps the history (when each part was built, what was measured then, what was tried and dropped). Read the section for a system before changing it, and keep it in sync in the branch that changes it.

Live path: `main` picks a game from the game registry (`app/game_registry`: the demo, or another named with `--game`), makes it, then creates an `Application` (the engine). The `Application`:

- drives the game through the `Game` hooks,
- fills `Input` from the `Window`,
- and draws the `World` with the renderer (`VulkanRenderer`, the engine's own Vulkan 1.3 renderer).

## Settings

Settings: every value a game, the debug UI or a player may tune lives in one settings struct owned by its system, read every frame unless noted, with defaults that are the engine's values. Fixed capacities (shader array sizes, atlas size, tile counts) stay compile-time constants.

**The settings structs**

- `EngineSettings` (`engine/engine_settings.h`) is owned by `Application` (constructor argument, `getSettings()`). Its fields:
  - `ticksPerSecond` 120 and `maxFrameSeconds` 0.25, both read only at start-up;
  - `targetFPS` 120, 0 = unlimited;
  - `presentMode`, applied when it changes;
  - `framesInFlight` (2), applied when it changes;
  - `gpuValidation`, read at start-up and passed to the renderer's constructor (Vulkan's validation layers when installed; MSYS2's, which the makefile points the loader at; see VulkanRenderer). It defaults to `isDebugBuild` from `core/build_info.h`, which the makefile sets by defining `ENGINE_DEBUG_BUILD` in debug builds only;
  - and `render`.
- `RenderSettings` (`render/gpu/render_settings.h`) = `LightPrioritySettings lights` + `ShadowSettings shadows` + `BloomSettings bloom` + `ToneMappingSettings toneMapping` + `DisplaySettings display` + `GlobalIlluminationSettings globalIllumination` (render/gpu/ddgi.h; see Light probes). It is passed to `VulkanRenderer::render(frame, settings)` each frame.
- Camera controllers each own a settings struct (`FreeFlyCameraSettings`, `FirstPersonCameraSettings`, `MobaCameraSettings`), passed to the constructor and editable through `getSettings()`.
  - The look ones nest the shared `LookSettings`: `radiansPerPixel`, `sprintMultiplier`.

**Reaching them**

- Games cannot reach `EngineSettings` yet (it pulls in `render/` headers). `main` uses the defaults, and the saved settings file is loaded over them (see Debug UI and the settings file).
- Every settings struct has a `visit(Settings&, SettingsVisitor&)` next to it, in its system's `.cpp` (`RenderSettings` in `render/gpu/render_settings.cpp`, `EngineSettings` in `engine/engine_settings.cpp`).
- The demo's are reached through `Game::visitSettings`.

## Debug UI and the settings file

One description of every tunable value, walked by several visitors.

### The visitor

`SettingsVisitor` (`core/settings_visitor.h`) has public `field` overloads, and a visitor implements the protected `visitFloat`, `visitInt`, `visitBool`, `visitChoice` and `visitGroup`.

- **Field kinds:**
  - `float` (`FloatLimits`), `int` (`IntLimits`), `bool`;
  - `double` (seen as a float, written back only when changed, so looking never rounds it);
  - `std::uint32_t` and `std::uint64_t` (seen as ints);
  - `choice` for enums whose values are 0, 1, 2... with a `std::array` of names;
  - and `group` (a label and a callback).
- **Limits:**
  - `min`/`max` are hard limits (the file clamps to them, tests check defaults lie within them);
  - `sliderMin`/`sliderMax` the useful range (default the hard limits; a field without finite hard limits must give them);
  - `unit` (shown after the number);
  - and `applies` (`FieldApplies::EveryFrame` or `AtStartup`).
- `SettingsWalk` is a `std::function<void(SettingsVisitor&)>`.
- **File keys** come from labels (`settingKey`: words in lower camel case, the first lowered whole: "GPU validation" is `gpuValidation`, "Target FPS" `targetFPS`), joined with dots through groups (`engine.render.bloom.strength`). Keep labels named after their members.
- Hard limits that the renderer also checks share a constant (`maxBloomStrength`, `BLOOM_LEVEL_LIMIT`), and a test checks the ends pass `validateBloom`/`validateToneMapping`.

### The settings file

`engine/settings_file` holds the functions that read and write it:

- `captureSettings(walk)` lists every field as `SettingEntry {key, value}`: floats as the shortest text that reads back exactly (`std::to_chars`), bools `true`/`false`, enums by name.
- `applySettings(entries, walk, StartupFields)` sets the named fields:
  - clamping to hard limits;
  - skipping text that does not read as its kind, and ignoring unknown keys, with a warning for each;
  - and leaving start-up fields alone with `StartupFields::Keep`.
- `formatSettingsText(current, defaults, kept)` writes a two-line comment, each `key = value` whose text differs from the default's, then the `kept` entries as they are.
- `separateSections(entries, walk)` splits saved entries into:
  - `own`: their first key part is a section the walk has, or they have no dot, so a stray key is still warned about;
  - and `others`: a section the walk lacks (another game's).
- `parseSettingsText` reads `key = value` lines (blank lines and `#` comments skipped, `\r\n` accepted, malformed lines warned with their number).
- `findLimitProblems` (tests).
- `engineAndGameSettings(settings, game)` is the whole tree: `Engine`, then a group named by `Game::title()` (keys `demo.grenade.throwSpeed`, `lightLab.sun.solarHours`), so every game has its own section of the one shared file.
- `readSettingsFile` (nothing when the file is absent) and `writeSettingsFile`.

**Loading, saving and resetting**

- The file is `savedSettingsPath` = `settings.cfg` in the working directory (git-ignored).
- `Application::captureDefaultsAndLoad` captures the defaults, then loads the file's own sections over them, leaving other games' alone. Problems are logged with `SDL_Log`, never fatal.
- Save writes the changed values plus the other games' sections, read afresh from the file.
- Reset applies the captured defaults with start-up fields kept.

### The debug UI window

`DebugUi` (`engine/debug_ui`) owns the ImGui context and the SDL3 platform backend, and starts the renderer's half (`VulkanRenderer::startDebugUi`: ImGui's Vulkan backend, see VulkanRenderer; the SDL3 backend starts with `ImGui_ImplSDL3_InitForVulkan`).

- Its destructor stops the renderer's half first (`stopDebugUi` waits for the GPU to go idle).
- No `imgui.ini`, keyboard navigation off (the game keeps its keys), sizes and font scaled by `SDL_GetWindowDisplayScale`.
- `build(walk, render, requests)` returns `nullptr` while closed (nothing runs). Open, it:
  - runs the backends' `NewFrame`s (the renderer's through `beginDebugUiFrame`);
  - shows one window "Settings (F5)" with the quality preset combo (`Custom` when `matchingQualityPreset` finds none) and the upscaling mode combo (which sets `renderScale`; `Custom` when `matchingUpscalingMode` finds none);
  - has Save and Reset buttons (reported in `DebugUiRequests`, carried out by `Application`, which sets the status line);
  - then runs the walk through `ImGuiSettingsVisitor`. Top-level groups are collapsing headers (open), deeper ones tree nodes, each with its own ID scope. Floats and ints are sliders (`%.4g` plus unit), clamped to the hard limits after Ctrl+click typing. Start-up fields are disabled with a tooltip.

### Quality presets

Quality presets (`render/gpu/quality_preset`): `QualityPreset` Low, Medium, High, Ultra (`qualityPresetNames`). One table sets these, and taste settings are never touched:

| Preset | Shadowed point / spot light caps | Both texel targets | Sun's shadow distance | Cascades | Cascade resolution | Bloom levels | Probe rays / fast rays | Sharing |
|---|---|---|---|---|---|---|---|---|
| Low | 8, 4 | 0.1, 0.05 | 60 | 3 | 2048 | 4 | 32, 32 | 0.5 |
| Medium | 24, 8 | 0.06, 0.03 | 120 | 3 | 2048 | 5 | 64, 64 | 0.3 |
| High | = Ultra | | | | 4096 cascades in the grown 384 MB atlas | | 256, 256 rays | sharing 0.1 |
| Ultra | = `RenderSettings{}`, the default | | | | | | 256, 256 rays | no sharing |

The table also sets reflections (off on Low; Medium 32 steps and a 64-texel probe, High a 128-texel one, Ultra 64 steps and 256), ambient occlusion (off on Low; slices and steps Medium 2 and 3, High 2 and 4, Ultra 3 and 6) and, on Ultra only, the ray-traced features: ray-traced reflections and mirrored lights (`ultra-rt-defaults`, Oct 23 2026, when Ultra's probe rays went from 1024 back to 256, High's).

- Low: 2 cascades of 1024 left 30 cm walls with ragged sunlit edges, and 1024 saved no memory.
- High has been the default since `gi-quality-presets`, before which Ultra was the default and High had 2048 cascades.
- `applyQualityPreset` and `matchingQualityPreset` read the same table.

### Drawing the UI

Drawing: `VulkanRenderer::render(frame, settings, ui)` with `ui` = `ImDrawData*` or null.

`VulkanOverlay` (`render/vulkan/vulkan_overlay`; its composite pipeline is remade with the screen pipelines when the swapchain format changes) does it in two steps:

1. `drawUi`, called in `drawScene` before the shadow pass, while no pass is open, uploads ImGui's changed textures and draws the UI with `ImGui_ImplVulkan_RenderDrawData` into the overlay image: `R8G8B8A8_UNORM` (`vulkanOverlayFormat`), the frame's size, cleared to transparent, so it ends up premultiplied.
2. `composite`, after the debug lines in the tone mapping pass, draws `fullscreen_triangle.vert` + `overlay.frag` (sampler 0 the overlay, nearest; uniform `OverlayData` with the white level) with blending `ONE, ONE_MINUS_SRC_ALPHA`. Each texel's colour is:
   - divided by its alpha (capped at 1),
   - decoded from sRGB (`srgbToLinear` from `dither.glsl`),
   - scaled by alpha and `DisplayOutput::whiteLevel`.

This is mirrored in `render/gpu/overlay_composite` (`overlayOutput`, `blendOverlay`).

Dear ImGui 1.92.9b is vendored unmodified in `external/imgui` (core files, `imgui_impl_sdl3`, `imgui_impl_vulkan`; `VERSION.txt`). The makefile adds its `.cpp` files to the engine sources and turns off `-Warray-bounds` for them only (GCC's false positives at `-O2`).

## Golden images

Golden images (`engine/golden`, `engine/golden_capture`, `VulkanRenderer::capture`) check that the renderer keeps drawing fixed scenes the same. The references were made on SDL_GPU before the Vulkan port, and the port matched them bit for bit.

### The views

A game lists its views with `Game::goldenViews()` (`GoldenView` in `engine/golden_view.h`), and sets one up in a world fresh from `onInit` with `Game::prepareGoldenView`.

- A `GoldenView` has:
  - `name`: lower-case letters, digits and dashes (`isGoldenViewName`);
  - `simulationSeconds`;
  - `hdr`.
- **The demo** sets a free camera at a fixed pose via `PlayerControls::poseFreeCamera`, and switches lamps and flashlight to the view's choice. Six views: `start`, `start-hdr`, `gallery`, `lamp-field`, `floodlights`, `moon`.
- **The light lab** sets the sun's solar time and day, a free camera pose, the flashlight and the lamp. Eight views:
  - `towers`, `overview`, `sunlit-room`, `sunset-hdr`;
  - `sun-disc` (the camera facing the sun wherever it stands at that hour);
  - `flashlight` (the dark back room lit by the flashlight, a fast light, so the probes' fast atlas is in a golden image);
  - `back-room-lamp` (the back room lit by its ceiling lamp);
  - `moonlit` (day 88 at 21:00, a waxing gibbous moon: the camera facing the moon's bearing with the moon near the top of the view, the stars, the moonlit sky and the moon's shadows).
- `advanceToGoldenView(game, world, view, ticksPerSecond)` prepares the view, runs `round(seconds × rate)` ticks through `runFixedUpdate` and one `onUpdate` with an empty `Input`. `golden_views_test` uses it to check each view is deterministic.

### Capturing

`runGoldenImages(makeGame, gameFolder, mode)` runs them. From `main` it is `--golden-save` or `--golden-check`, for every registered game or only the one named with `--game`, each in a folder named by its registry name (`make golden-save`, `make golden`).

- It builds a fresh game and `Application` per view:
  - window 640x360, `EngineSettings{}`, `SavedSettings::Ignore`, so `settings.cfg` never changes a capture;
  - and a cold shadow cache and light history like a first frame.
  - With `fitCascadesToDepth` the renderer first draws the frame once to measure its depths for the sun's cascades, or 960 times (8 s) when it has light probes, and then undoes everything else those frames changed (`drawFramesBeforeCapture`).
- Then it calls `Application::captureGoldenView`: advance, upload new assets, `describeFrame(1, tick)` (shared with `render`), then `VulkanRenderer::capture(frame, settings, 1280, 720, goldenDisplayOutput(hdr))`.
- `capture`:
  - runs `drawScene` (shared with `render`: light priority, shadows, main pass, bloom) into frame images resized to 1280x720;
  - tone maps into an off-screen target (`captureFormat`: `R8G8B8A8_SRGB` like the SDR swapchain, or `R16G16B16A16_SFLOAT` scRGB) with a tone mapping pipeline built for that format (`createToneMapPipeline`);
  - copies the scene image and the target into a read-back buffer, waits for the GPU and returns a `CapturedFrame` (`render/captured_frame.h`: two `RawImage`s);
  - restores the display output so the next real frame is unaffected, and draws no debug lines or UI.
- `goldenDisplayOutput` fixes the screen: SDR, or HDR with SDR white at scRGB 2.5 (200 nits) and headroom 4.

### Files and comparing

- Files: `tests/golden/<goldenFolderName(GPU name)>/<game>/<view>-scene.png` and `<view>-final.png`.
- Half-float images are packed losslessly into 8-bit PNGs twice as wide (`packHalfImage`, `unpackHalfImage` in `core/raw_image`; `saveRawImagePng`, `loadRawImagePng` in `assets/image_loader`).
- A check writes its captures and, for any image that differs, a heatmap (black equal, yellow within tolerance, red beyond) to `build/golden/<GPU>/<game>/`, prints one line per image and exits with 1 if any failed.
- `compareImages` (`core/image_compare`) counts differences in steps: bytes, or half-float ulps through `halfStepDistance` in `core/half_float`, where +0 and -0 are equal and NaN is never close.
- `ImageTolerance` defaults: at most 1 step, in at most 0.1% of pixels.
- The references are valid only for the GPU and driver that made them. On this one two runs, and the debug and release builds, match bit for bit.

## GPU tests and shader libraries

The shaders' maths lives in pure-function libraries in `assets/shaders/`, included by the passes and by the GPU tests, each mirrored in C++.

### The libraries

| Library | Functions | C++ mirror |
|---|---|---|
| `brdf.glsl` | `ggxDistribution`, `smithVisibility`, `schlickFresnel`, `viewCosine`, `ShadingPoint` and `prepareShading(baseColour, metallic, roughness, normal, toCamera, tableEntry)`, `facesLight`, `addLight`, `shadeAmbient`, `shadeProbeAmbient` | `render/gpu/brdf` |
| `normal_mapping.glsl` | `decodeNormalTexel`, `perturbNormal` | `render/gpu/normal_mapping` |
| `lighting.glsl` | `distanceFalloff`, `spotCone`, `pointShadowFace` | `render/gpu/light_falloff` and `shadow_map` |
| `soft_shadow.glsl` | `linearShadowDepth`, `blockerSearchTexels`, `penumbraTexels`, the sun's `boxShadowDistance`, `sunBlockerSearchTexels` and `sunPenumbraTexels`, and the filter limits | `render/gpu/soft_shadow` |
| `tone_mapping.glsl` | the curves, `applyCurve`, `toneMap(linear, exposure, peak, curve)` | `render/gpu/tone_mapping`, whose `toneMap` also clamps negative light, which `tone_map.frag` does before mixing in bloom |
| `bloom.glsl` | `karisWeight`, `mixBloom` | `render/gpu/bloom` |
| `dither.glsl` | `linearToSrgb`, `srgbToLinear`, `interleavedGradientNoise`, `ditherForEightBits(linear, noise)` | `core/srgb`, `core/dither` |
| `overlay.glsl` | `overlayOutput` | `render/gpu/overlay_composite` |
| `clusters.glsl` | the `ClusterGrid` struct, `toClusterView`, `clusterSlice`, `sliceStartDepth`, `clusterIndex`, `clusterOfPixel`, `clusterBounds`, `sphereTouchesBox`, `coneTouchesSphere`, `clusterHeatRamp`, `clusterHeatColour` | `render/gpu/clusters`, see Clustered shading |
| `cascades.glsl` | `cascadeEdgeDistance`, `cascadeBlendWeight`, `cascadeDitherKeeps`, `sunShadowBias` | `render/gpu/cascades`, see Cascaded sun shadows |
| `depth_range.glsl` | `includeDepth` | `render/gpu/depth_range` |
| `ray_tracing.glsl` | `cameraRayDirection`, `hitCornerWeights` | `render/gpu/ray_tracing`, see Ray tracing |
| `ddgi.glsl` | `octahedralEncode`, `octahedralDecode`, `probeTexelDirection`, `probeBorderSource`, `probeBorderTexelCount`, `probeBorderTexel`, `sphericalFibonacci`, `probeRayDirection`, `irradianceRayWeight`, `distanceRayWeight`, `distanceForUpdate`, `blendProbeValue`, `seededProbeTexel`, `probeSlot`, `probeGridCoord`, `probePosition`, `probeTileOrigin`, `probeAtlasUv` | `render/gpu/ddgi`, see Light probes |
| `ddgi_sampling.glsl` | `probeSelfShadowBias`, `probeCell`, `probeCellCorner`, `probeTrilinearWeight`, `probeBackFaceWeight`, `probeVisibility`, `crushProbeWeight`, `probeCornerWeight`, `probeCascadeWeight`, `addProbeSample`, `finishProbeBlend`, `pointLightAtHit`, `spotLightAtHit`, `hitLightStrength`, `keepBrightestLight` | `render/gpu/ddgi_sampling` |
| `probe_cascades.glsl` | the `ProbeCascade` record, `cascadeProbePosition`, `cascadeProbeNumber`, `cascadeWeight` | `render/gpu/probe_cascades` |

**Rules for a library**

- Each has an include guard and includes the `*_shared.h` headers it needs.
- Library functions take everything as arguments: no texture reads, no `gl_FragCoord`, no derivatives (compute shaders have none of them). So `triangle.frag` reads the BRDF table (`sampleBrdfTable`) and `tone_map.frag` the pixel's noise, and pass them in.
- Moving the functions into libraries left every golden image bit for bit the same.
- Only `.vert`, `.frag` and `.comp` files are compiled (for Vulkan 1.3, `--target-env=vulkan1.3`, which subgroup operations need); `.glsl` files are included.

### The GPU test program

`make gputest` (`make gputest-release` for the release build):

- builds `build/gputests.exe` from `tests/gpu/*.cpp` and the engine objects (not part of `tests.exe`, so `make test` needs no GPU);
- compiles `tests/gpu/shaders/*.comp` (with `-Isrc -Iassets/shaders -Itests/gpu`; glslc's dependency files in `build/obj/shaders/gpu_tests/`);
- runs it, then `game.exe --golden-check`.

`gpu_test_main` makes a windowless `VulkanDevice{nullptr, true}` (validation on; any validation error fails the run), then runs the tests below in order, prints the report and exits with 1 on any failure.

**1. One test per library.** One `gpuTestXxx(runner, report)` per library: `brdf_gpu_test`, `normal_mapping_gpu_test`, `lighting_gpu_test`, `soft_shadow_gpu_test`, `tone_mapping_gpu_test`, `bloom_gpu_test`, `dither_gpu_test`, `overlay_gpu_test`, `clusters_gpu_test`, `cascades_gpu_test`.

**2. `gpuTestDepthRange`** (`depth_range_gpu_test`): the real `VulkanDepthRange` pass on depth images of known values uploaded through a buffer, each compared bit for bit with the CPU range:

- sizes cut off mid-group;
- a lone surface in the last pixel;
- the near plane and the smallest normal float (depth images may flush denormals);
- and sky only.

**3. `gpuTestRayTracing`** (`ray_tracing_gpu_test`): `ray_tracing.glsl` through the runner, then, on a GPU with ray queries, real structures:

- a cube, a sphere and a plane placed five times (turned and stretched, mirrored, one visual-only, a floor);
- traced by `ray_query_test.comp` (`RAY_SCENE_SET` 0, its buffers by address in push constants);
- compared ray by ray with the CPU's `raycastTriangle` over the same triangles:
  - hit or miss, instance, triangle and facing exactly;
  - distance and interpolated normal within a tolerance growing with 1/cosine, since rounding of where a plane is moves a grazing hit along the ray;
- 2,000 rays from in front of the scene and from inside the cube, each solid-only or seeing everything.

**4. `gpuTestDdgi`** (`ddgi_gpu_test`): `ddgi.glsl` and `ddgi_sampling.glsl` through the runner, then, on a GPU with ray queries, the real `VulkanDdgi` passes on scenes the CPU works out ray by ray with the same directions (frame 0's rotation), atlases read back as half floats:

- the furnace: 3 x 2 x 2 probes under an empty sky store the sky in every texel and the longest distance;
- a probe sealed in a box: back faces only, so no light, a fifth of each wall's distance;
- a probe over a white floor lit straight down by a sun: rays down bring back its radiance, rays up the black sky;
- and the same floor lit by two lamps with one kept per hit: rays down bring back the brighter lamp's light where they land;
- every border texel an exact copy of its source;
- then the cascades, two of 13 x 13 x 13 moved a metre after 8 frames:
  - in the furnace: every probe still the sky, the plane brought in seeded with the coarser cascade's light, its history marked restarted with the seed's frames plus one, every other probe's kept;
  - and over a static floor with relocation and states on: the plane brought in off on the frame it comes in, its records back the next frame, the rows by the floor vigilant and seeded.

How the DDGI test sets up its scenes:

- A scene's frame set is written as the renderer writes it (`BindlessTextures` with white at slot 0, `SceneLayout`, `SceneBuffers`, dummy images for the shadow atlas and BRDF table), and a speck far below fills the top-level structure.
- The probe passes test also writes the probe fields and binds its own fresh atlases as set 1's.
- The stored values may sit a whole half-float step low, since Vulkan lets a GPU round toward zero when it stores 16-bit floats. This was seen on the RTX 5080: 0.4% low on the floor light while irradiance was stored as its fifth root, five times one step.

### The runner and its checks

- `GpuFunctionRunner::run(shader, function, inputs, inputsPerCase, outputsPerCase)` (`tests/gpu/gpu_function_runner`):
  - inputs as `GpuVec4` (`std::array<float, 4>`, 16 bytes like a std430 `vec4`) in an upload storage buffer, outputs in a read-back storage buffer;
  - one descriptor set from a one-set pool reset per run;
  - push constants `{count, function, inputsPerCase, outputsPerCase}`;
  - `vkCmdDispatch(workgroupCount(count, GPU_TEST_GROUP_SIZE))`, then a memory barrier compute shader storage writes to host reads, `ImmediateCommands::run` (submit and wait) and `invalidate`;
  - host writes need no barrier: submitting makes them visible (after `flush`);
  - one `ComputePipeline` per shader, made on first use.
- Test shaders: `tests/gpu/shaders/gpu_test.glsl` (the buffers, the push constant block `TestCase`, `local_size_x = GPU_TEST_GROUP_SIZE` (64), `outsideTheCases`, `testInput(index)`, `testOutput(index, value)`) and one `<library>_test.comp` per library, which switches on `function` and calls the library.
  - Function numbers and the group size: `tests/gpu/gpu_test_functions.h` (macro-only, read by both sides).
- `tests/gpu/gpu_check`:
  - `CaseRandom` (xorshift32, the same inputs every run; `between`, `direction` uniform on the sphere);
  - `gpuVec4`, `vec3From`, `gpuTestShader(name)` (`tests/gpu/shaders/<name>.comp.spv`);
  - and `GpuCheckReport`: `section`, `check(name, tolerance, expected, actual)` for floats and `Vec3`s, grouped by name per section; `print` shows per group the values checked, pass or FAIL, and the worst comparison's share of its tolerance with both values; `failedGroups`.

**Tolerances** (`core/float_tolerance`):

- `FloatTolerance{absolute, relative}`, allowed difference:

  `absolute + relative * |expected|`

- `toleranceUsed`: 0 equal, including equal infinities; infinite for a NaN or a difference where nothing is allowed.
- `withinTolerance`: used share at most 1.
- `ToleranceTally` (`checked`, `failures`, `worstUse` with `worstExpected` and `worstActual`; `add` with the tally's tolerance or one per comparison).
- The tests use:
  - `arithmeticTolerance` (1e-6 absolute, 1e-5 relative: + - * / and sqrt);
  - `transcendentalTolerance` (1e-6, 1e-4: pow, exp2, log2);
  - and `exactTolerance` (cube faces, copies).
- Two follow the problem:
  - `interleavedGradientNoise` is compared around the circle (it wraps at 1) within 4e-3, since at 4K pixel coordinates its inner product keeps only a few digits;
  - `addLight` specular adds `4 * 4 epsilon / d` relative, with d = 1 - nDotH^2 (1 - alpha^2) GGX's denominator, because near a sharp highlight's peak a float step in nDotH changes the result by about 4 delta / d.

**What is compared:**

- GGX over 41 halfway cosines by 11 roughnesses, Smith, Fresnel (cosines past both ends);
- 400 random surfaces through `prepareShading` (the table entry looked up on the CPU and passed in), `addLight`, `shadeAmbient`, `shadeProbeAmbient` and `facesLight`:
  - a fifth of them with the eye or light behind the surface;
  - `facesLight` exactly, except within 1e-6 of grazing, plus one light exactly along the surface;
- normal texels and 300 perturbed frames (plus a zero one);
- falloff, spot cones, cube faces (random and on every edge and corner, exactly);
- the soft shadow arithmetic over both clamps;
- every tone mapping curve over 2^-12 to 2^10 at two exposures and peaks;
- the Karis weight and bloom mix, the sRGB curves, noise across a 4K screen, the dither;
- and overlay texels at every fifth alpha step and three white levels.
- Texture filtering (bloom's filters, PCSS's sampling, the BRDF table lookup) is left to the golden images.

**Measured:** first results on the RTX 5080: every group within 16% of its tolerance except the noise (40%). Planting a bug (Fresnel's power 5 changed to 4, the falloff window's fourth power to a cube) failed at 4,000 to 27,000 times the tolerance.

### Compute pipelines

Compute pipelines: `ComputePipeline` (`render/vulkan/vulkan_compute`, from `ComputePipelineDescription{shader, storageBuffers, pushConstantBytes, sampledImages, storageImages, sharedLayout}`).

- It makes set 0 with storage buffers at bindings 0.., then `sampledImages` combined image samplers, then `storageImages` storage images (read and written in place), a push constant range for the compute stage, and calls `vkCreateComputePipelines`.
- Or, with `sharedLayout` (not owned), it takes a layout made elsewhere and builds only the pipeline (`getResourceLayout` is then null).
- Accessors: `get`, `getLayout`, `getResourceLayout`, `getPushConstantBytes`.
- `workgroupCount(count, groupSize)` (`render/gpu/compute_dispatch`, tested in `compute_dispatch_test`): groups to cover `count` items, rounded up without overflow.
- Users:
  - the GPU tests (their own layouts);
  - the renderer's cluster pass (the scene's layout; see Clustered shading);
  - the depth measurement (its own layout with a sampled image; see Cascaded sun shadows);
  - and the light probes (the trace with a shared layout, the updates with a storage buffer and a storage image; see Light probes).

## Application (the engine)

`Application` (`engine/`) runs the main loop (fixed timestep, frame cap) for a borrowed `Game` (declare the game first so it outlives the engine). It owns the `Window`, the renderer (`VulkanRenderer`), `Input`, the `World` and the `AssetManager`.

**Members, in order:**

- the borrowed `game`;
- `settings` and `defaultSettings` (initialised by `captureDefaultsAndLoad`, so the saved file is applied before the renderer reads `gpuValidation` and the timestep its rate);
- `window`, `renderer`, ...;
- `debugUi` (after the window and renderer, so it is destroyed first).

**Each frame:**

- After the events, `updateDebugUi` handles F5 and builds the UI before `game.onInput`, so a changed setting applies that frame. Its draw data goes to `render` and on to `VulkanRenderer::render`.
- While the UI is open the mouse is freed (`applyMouseMode` asks for mouse look only when the game wants it and the UI is closed).
- Before each frame it uploads every mesh and texture the `AssetManager` gained since the last frame (the ones past the renderer's counts), so games can load and spawn at any time.
- It applies `game.wantsMouseLook()` to the window only when it changes (re-applying would undo Escape).

## The Game interface, debug drawing, culling and render stats

### The Game interface

`Game` (`engine/game.h`) has a `title()` (for people: the window title, after "My Engine: ", and the settings section) and hooks.

- Called each frame in this order:
  1. `onInput(world, input)` once;
  2. `onFixedUpdate(world, tickSeconds, simulationSeconds)` per tick (no `Input` parameter on purpose);
  3. `onUpdate(world, input, frameSeconds, alpha)` once.
- Plus `onInit(world, assets)`, `getCamera()` (the engine sets its aspect ratio), `wantsMouseLook()` and optional `getLightFocus()` (where lights are ranked, default the camera position).
- Optional `onDebugDraw(const World&, DebugDraw&, alpha)` (called every frame after `onUpdate`) and `wantsDebugView()`.

### Debug drawing

Debug drawing: `DebugDraw` (`scene/debug_draw`) collects `DebugLine`s (`line`, `box(bounds, transform)`, `sphere` as three circles, `capsule`).

- `Application` clears it every frame, then:
  - adds `drawBoundingBoxes` (`engine/debug_views`, from `boundsByMesh`, filled with `meshBounds` as meshes upload) and `drawRenderStats` (last frame's `RenderStats`, `render/render_stats.h`, as text in the top-left) when the game wants the debug view;
  - calls `onDebugDraw`;
  - and puts the lines in `FrameDescription::debugLines` and `debugScreenLines`.
- `DebugDraw::screenLine` adds lines in window pixels (top-left origin).
- `debugText`/`debugTextWidth` (`scene/debug_text`) write text with a stroke font (A-Z, 0-9, : - / . ?, on a 4x6 grid; lower case drawn as upper, unknown characters as ?), a stopgap until phase 9's font rendering.
- The renderer draws world lines with the camera and screen lines with a flipped pixel orthographic matrix (`Mat4::orthographic` accepts flipped boxes, rejects empty ones).
- `VulkanRenderer` hands them to its `VulkanDebugLines` (`render/vulkan/vulkan_debug_lines`), which draws them last as a line list with no depth test, so they show through everything. Colours are linear.
  - It owns the line pipeline and a growing upload vertex buffer per frame in flight.
  - `upload` writes the vertices from `buildDebugLineVertices` straight into it before the render passes.
  - `draw` runs in the tone mapping pass, after the scene is tone mapped, so line colours are exact.
- `World::each` has a const overload that passes const components.

### Culling

- Culling: `Frustum::fromClipMatrix(viewProjection, ClipDepth)` (`math/frustum`; six inward unit-normal planes from the matrix rows, either depth convention; the renderer's matrices are `ZeroToOne`) and `intersectsSphere`, plus `corners` and `mayIntersect` (false only when one view lies wholly outside a side of the other, tested both ways).
- Each `DrawItem` has a `BoundingSphere bounds` (radius < 0 = unknown, never culled).
- `Application` fills it with `attachBoundingSpheres(frame.draws, boundsByMesh)` (`scene/culling`: the box's centre through the model matrix, half-diagonal times the largest axis scale).
- The renderer draws only `visibleDraws(draws, cameraFrustum)` in the main pass and only `shadowCasters(draws, tileFrustum)` in each shadow tile.
- Shadow passes must never use the camera's frustum (off-screen casters still throw shadows into view).

### Render stats

`getLastFrameStats()` returns `RenderStats`, shown on the debug readout's lines:

- drawn, culled, shadowDrawn, shadowCulled, and point lights total, in view, lit and shadowed, shown as a third line of the debug readout;
- then shadowTilesDrawn, shadowTilesCached and shadowTilesSkipped, a fourth line, ending with `sunShadowDistance` (SUN TO, where the sun's cascades end) when a sun is shadowed;
- then `gpu`, the GPU timings per pass, see VulkanRenderer;
- then `videoMemory`, on `videoMemoryReadoutLine` (5). `videoMemoryText` writes "VRAM used / budget MB  ENGINE n MB" in megabytes of 2^20 bytes, or "VRAM ENGINE n MB  NO DRIVER BUDGET" without the driver's figures.

**Video memory.** `render/gpu/video_memory` (`HeapMemory`, `VideoMemory`, `summariseVideoMemory`) adds up the device-local heaps, so system memory the GPU can reach is left out and an integrated GPU's one heap counts.

- It is filled at the start of each `drawScene` from `VulkanDevice::getHeapMemory` (VMA's `vmaGetHeapBudgets`), after `startMemoryFrame` (`vmaSetCurrentFrameIndex` with a frame count, which makes VMA fetch the driver's figures again).
- The engine's share is each heap's `blockBytes`, the memory VMA took from Vulkan.
- `usage` and `budget` are the driver's for the whole process with `VK_EXT_memory_budget`, which the device turns on when present (`reportsMemoryBudget`), and VMA's estimates without it.

### The games' side

- The demo toggles its debug view with F4 (bounding boxes, the culling readout, a sphere in its colour at each point and spot light, a beam line for spotlights, and a line to the walk target).
- Always run ticks through `runFixedUpdate`, which saves previous transforms first; tests use it too.
- `DemoGame` (`games/demo/`) holds the player controls (camera, control modes, body), the player and marker entities, and all demo input rules.
- `LightLabGame` (`games/lightlab/`) is the second game (see Games and the light lab).
- `window` is declared first and `renderer` second: members are destroyed in reverse order, so the renderer goes before the window it draws into, and the GPU device outlives every uploaded mesh.

## Window

`Window` (`platform/`) owns SDL video and the `SDL_Window`. It reports raw facts only; game rules live in game code.

- It handles event polling into `Input`, quit, focus loss (releases everything), click to capture (mouse look) or confine (MOBA cursor), Escape to release, Alt+Enter fullscreen.
- While the debug UI has started (ImGui's SDL backend is running), every event goes to `ImGui_ImplSDL3_ProcessEvent` first, releases included.
- While it is open (`setDebugUiOpen`), key presses and mouse button presses ImGui wants (`WantCaptureKeyboard`, `WantCaptureMouse`, read once per frame) do not reach `Input`, and a click it takes neither captures nor confines the mouse.
- After the events it records the cursor position, window size, keyboard focus, mouse capture and cursor confinement into `Input`, so games read those facts from `Input` instead of the window.

## The renderer, shadows and light priority

The renderer (`VulkanRenderer`, `render/vulkan/`, next section, with the API-free parts in `render/gpu/`) draws with SPIR-V shaders into a window it does not own. It owns the device, pipelines, frame images, and GPU copies of meshes and textures in arrays indexed exactly like the handles (`uploadMesh`, `uploadTexture`).

### Each frame

- Each frame it gets one call, `render(frame, settings)` (the frame plus `RenderSettings`), with a `FrameDescription`.
  - `scene/frame_description`: a copy of the camera, the `FrameLighting` and a list of `DrawItem`s, each `{MeshHandle, model matrix, Material}`.
  - It is built by `buildFrame(world, camera, alpha)` from every visible `ModelRenderer` with a `Transform` (one item per part, interpolated).
- Having the whole frame lets it draw the scene more than once (shadow maps) and cull.
- The per-frame steps are private: `beginFrame`, `drawScene`, `prepareShadowDraws`, `writeScene`, `drawShadows`, `drawMainPass`, `drawBatches`, `drawToneMap`, `submitFrame`, and `abandonFrame` to submit after an error (see GPU scene below).

### Shadows: the atlas and its tiles

Shadows: every light casts shadows unless its `castsShadows` is off (default on for directional, point and spot lights). `ModelRenderer::castsShadows`, default true, carried on each `DrawItem`, decides which meshes block light.

`planShadows(lighting, camera)` (`render/gpu/shadow_map`) hands out square tiles of one D32 depth atlas.

- **Atlas size:** 8192 x 4096, 128 MB; or 8192 x 12288, 384 MB, with the default 4096 cascades (`shadowAtlasSizeFor(ShadowSettings)`, see Cascaded sun shadows).
  - It is one 2D image because SDL_GPU could not draw into one layer of an array texture, kept through the port.
- **Tiles**, numbered sun first, then spotlights, then point lights, in slot order:
  - 1 per cascade of the brightest directional light (`maxShadowedDirectionalLights` = 1, up to `maxShadowCascades` = 4 texel-snapped boxes of `cascadeTilePixels(ShadowSettings::cascadeResolution)` each, see Cascaded sun shadows). Other directional lights still light, unshadowed.
  - 1 per spotlight (perspective down the beam, `spotShadowMatrix`).
  - 6 per point light (cube faces, `pointShadowFace` = largest axis, mirrored in the shader).
    - A face whose widest view misses the camera frustum, by `mayIntersect`, gets no atlas square and no draw. It keeps its place in `tiles`/`tileData` with size 0 and an empty rect, which the shader counts as lit, so `firstTile + face` still works.

### Shadow caching

Shadow caching: the renderer keeps two things between frames.

- A `ShadowAtlasLayout` (`render/gpu/shadow_atlas`):
  - `planShadows(lighting, camera, layout)` asks it for squares by `ShadowTileKey` = light kind, entity (slot when the entity is `Entity{}`) and face, and gets last frame's square back for the same size;
  - a buddy `ShadowAtlasAllocator` merges freed quarters;
  - on fragmentation it repacks everything once;
  - the two-argument `planShadows` uses a fresh layout.
- A `ShadowTileMemory` (`render/gpu/shadow_cache`): per square, the matrix and `shadowCasterSignature`, an FNV-1a hash of each caster's mesh index and model matrix.
  - `remember` forgets overlapped squares;
  - `forgetAll` after any render error.

`prepareShadowDraws` finds each tile's casters in two steps, and skips tiles whose matrix and signature match:

1. `castersInReach(draws, tile.reach)`: a bounding sphere against the light's range sphere, one test per caster, computed once for the consecutive tiles of one light;
2. then `shadowCasters(draws, candidates, frustum)` for the tile's own view.

A caster wholly beyond a light's range cannot block it where it shines. Only a soft shadow's wide filter could still reach one, where the falloff is all but zero: this changed 6 pixels of one golden image by at most 3 half-float steps.

`drawShadows` loads the atlas (kept between frames), resets each redrawn tile with `shadowClearPipeline` and skips the pass when nothing changed.

- `shadowClearPipeline`: `fullscreen_triangle.vert`, a viewport-covering triangle at the far depth `GPU_FAR_DEPTH` (0), also used by the tone mapping pass, compare `ALWAYS`.

### How many lights get shadows

- Up to 1 sun, 8 spot and 64 point lights (`maxShadowTiles` = 396). Lights past that still light, unshadowed.
  - With the cascades at 1024: at most 4 x 16 + 8 + 64 x 6 = 456 of the 512 smallest-tile places, so everything fits at the smallest size.
  - With 4096 cascades: 4 x 256 + 392 of the grown atlas's 1536, so the cascades keep their size.

### Tile size

`shadowTileSizeFor(halfWidth, range, texelTarget, largestSize)` picks a power of two from 256 px up to `largestSize`, so one texel covers about `ShadowSettings::texelTarget` = 0.04 units across the far end of the view.

- `largestSize` is 1024 `standardShadowTileSize` for point faces, 2048 `largestShadowTileSize` for spotlights.
- Spotlights use `spotTexelTarget` = 0.02, since their beams land at a slant short of their reach. The floodlights get 2048, the flashlight 512.
- If the total would overflow, `shrinkToFit` halves the least important (last) requests first, so every point light reaches the smallest size before a spotlight shrinks, and the sun's cascades shrink last, never below 1024.
- The `ShadowAtlasLayout` places them, largest first, with its buddy `ShadowAtlasAllocator` (power-of-two squares split into quarters and merged back), so they never leave gaps.
- `ShadowPlan::tiles` holds each `ShadowTile` (matrix, x, y, size in pixels) for the shadow pass's viewports.
- Matrices and offsets depend on the tile size (`pointShadowFaceMatrix`/`spotShadowMatrix` take it, default 1024).
- The `ShadowAtlasAllocator` starts as squares of its `ShadowAtlasSize::largestSquare`: 8 of 2048, or 6 of 4096 in the grown atlas.
- Perspective views are a little wider than needed so 3x3 PCF never leaves a tile.

### Light priority

`selectDrawableLights` (`light_uniforms`, with `canBeDrawn` per light) is the single rule for which lights reach the shader and in which slot, used by both `packLighting` and `planShadows`.

Before them, `drawScene` runs `prioritizeLights(lighting, frame.lightFocus, cameraFrustum, lightHistory)` (`render/gpu/light_priority`). It:

1. drops lights whose range sphere misses the camera frustum;
2. ranks the rest by `priority` (an int on `PointLight` and `SpotLight`, default 0; higher always wins), then by `lightImportance` at the focus:

   `intensity * brightest channel / (d² + r²)`

   with no range window, so far lights do not tie at zero; directional lights by brightness alone;
3. seats the top lights up to the seats (by default every light the buffers hold);
4. gives shadows to the top seated casters up to the shadow caps, switching `castsShadows` off on the others' copies, so `planShadows` needs no change;
5. and returns a `LightHistory` (entities in view, and each seat holder's `LightFade`: entity, light and shadow strength 0..1) that the renderer keeps.

The shader skips a light, and so its shadow lookups, for pixels beyond its range.

**`LightPrioritySettings`** (an optional last parameter):

- seats defaulting to `defaultPointSeats` and `defaultSpotSeats`, equal to the buffers' room since clustered shading, and clamped to the light buffers' room, `maxPointLights` (1024) and `maxSpotLights` (256);
- the shadow caps, clamped to theirs;
- plus `incumbentAdvantage` and `fadeSeconds`.
- Tests lower the point shadow cap to 16 to exercise shadow-only handovers.

**Handovers** fade over `fadeSeconds` (0.25; 0 hands over at once):

- A holder that is no longer wanted keeps its seat or shadow while fading out and frees it at zero.
- Then the wanted newcomer fades in: from 0 if it was on screen last frame, at full if not (a light just switched on or come into view appears at once).
- A light leaving the view is dropped at once.
- The strengths reach the GPU as `PlacedPointLight`/`PlacedSpotLight::fade` (multiplied into the radiance by `packLighting`) and `shadowFade` (`LightShadow::strength` in the plan, copied with `emitterRadius` for soft shadows into each light's record by `attachShadows`; the shader mixes the shadow towards lit).
- `prioritizeLights` takes the frame time, from `FrameDescription::frameSeconds` (set by `Application` from the timestep).
- Last frame's choices count `incumbentAdvantage` (1.25, `LightPrioritySettings`) times as much (hysteresis, between equal priorities only).
- `PlacedPointLight`/`PlacedSpotLight` carry their `Entity` for this (`Entity{}` never counts as last frame's).

**The light focus.** `FrameDescription::lightFocus` is the camera position from `buildFrame`, then `Application` sets it from `Game::getLightFocus()` (default: camera position). `DemoGame` returns `MobaCameraController::getLookPoint` in MOBA mode, the ground point the camera looks at.

### Shadow data on the GPU

- `ShadowPlan` holds one `LightShadow` per light the shader gets, in slot order (`directional`, `spots`, `points`: first tile, -1 for none; strength; emitter radius), which `attachShadows(GpuLights&, plan)` copies into the light records (see GPU scene).
- Per-tile data, `ShadowTileData` (112 bytes), goes to the scene's `ShadowTiles` storage buffer (std430, set 1 binding 6). It holds:
  - the column-major matrix;
  - the normal offset per unit of distance for perspective tiles, or fixed for boxes;
  - the atlas rectangle 0..1;
  - and `lens`: near, far and world size of a texel per unit of distance for perspective tiles; for a sun's cascade boxes 0, the box's depth, its texel's world size and its radius.
- Each `ShadowTile` also carries `reach`, the sphere its light lights (centre at the light, the range as radius; radius -1 for the sun's cascades, which reach everywhere).

**Uniform blocks must stay at or under 4096 bytes** (`maxUniformBlockBytes`, `render/gpu/uniform_limits.h`): `FrameUniforms` binds each block with a 4 KB range (SDL_GPU's window, kept from the port), and a shader reading past it reads outside the range with no error anywhere. Every uniform struct has a `static_assert` on it.

### The shadow pass and the lookups

- The shadow pass is one render pass with a depth-only pipeline (`shadow.vert`/`shadow.frag`, positions only, no culling), and one for cut-outs (`shadow_cut_out.vert`/`cut_out.frag`, with texture coordinates, throwing away the holes; see Cut-outs and double-sided surfaces).
  - No hardware depth bias: its slope-scaled part detached shadows from pillars seen edge-on by small tiles.
  - The viewport is moved to each tile, the tile's world view pushed as a push constant (`GpuShadowView`), and its casters drawn one instanced draw per mesh.
- The main shader samples the atlas as `sampler2DShadow` (fragment sampler slot 1, compare `GREATER_OR_EQUAL` for reversed-Z, linear).
  - It first offsets the point along its normal by 2.5 texels of its tile (`ShadowSettings::normalOffsetTexels`, the only bias; the sun's lookups move towards the sun instead, see Cascaded sun shadows).
  - It treats points outside a tile's view as lit.

### Soft shadows

Point and spot lights get soft shadows (PCSS, `softShadowFromTile`; the arithmetic is mirrored and tested in `render/gpu/soft_shadow`):

1. a 16-sample Poisson-disc blocker search in the atlas read as raw depths (`shadowDepths`, fragment sampler slot 2, a nearest non-comparison `shadowDepthSampler`);
2. the penumbra in texels (1 to 12):

   `emitterRadius * (receiver - blocker) / blocker`

3. then 16 comparison samples over it:
   - rotated per pixel by interleaved gradient noise;
   - clamped a texel inside the tile;
   - each compared against the receiver's plane (receiver plane depth bias from `dFdx`/`dFdy` of the camera-relative position, taken at the start of `main` in uniform control flow and projected through the tile).

`PointLight::emitterRadius` (0.05) and `SpotLight::emitterRadius` (0.025) are the glowing size for shadows, separate from `sourceRadius`, which only shapes the falloff (the demo flashlight's is 4.5).

### Odds and ends

- `Mat4::orthographic` builds the box projection (OpenGL depth convention like `perspective`).
- The renderer fits its camera copy's aspect ratio to the acquired frame, and `Application` then copies that ratio into the game's camera for click rays.
- It never reads files.

## VulkanRenderer

`VulkanRenderer` (`render/vulkan/`) is the engine's own Vulkan 1.3 renderer (phase 6b). It replaced SDL_GPU's `GpuRenderer` one pass at a time, with the same compiled shaders: all twelve golden images matched SDL_GPU's bit for bit (debug and release) before SDL_GPU was removed, so several of SDL_GPU's choices are kept on purpose (see Kept from SDL_GPU).

Public calls:

- the constructor (`SDL_Window*`, `validation`);
- `render`, `capture`;
- `uploadMesh`, `uploadTexture` and their counts;
- `startDebugUi`, `beginDebugUiFrame`, `stopDebugUi`;
- `setPresentMode`/`getPresentMode`, `getLastFrameStats`, `getDisplayOutput`, `getFrameAspectRatio` and `getDeviceName` (the golden folder's name).

### Libraries

Libraries (vendored unmodified, versions in each folder's `VERSION.txt`, matched to MSYS2's Vulkan headers 1.4.363):

- **volk** (`external/volk`, C) loads every Vulkan function from the driver at run time, so the makefile defines `VK_NO_PROTOTYPES` everywhere and nothing links against the loader.
- **vk-bootstrap** (`external/vk-bootstrap`) builds the instance, picks the GPU and makes the device.
- **VMA** (`external/vma`, implementation compiled once in `vk_mem_alloc.cpp` with dynamic function fetching, three warnings silenced for it) allocates memory.
- **ImGui's Vulkan backend** (`external/imgui/imgui_impl_vulkan.cpp`, built with `IMGUI_IMPL_VULKAN_USE_VOLK`).
- **Validation layers** come from MSYS2 (`mingw-w64-ucrt-x86_64-vulkan-validation-layers`, in `C:\msys64\ucrt64\bin`, which the loader does not search, so the makefile exports `VK_ADD_LAYER_PATH`).
- **FSR 3.1** (`external/fidelityfx`: AMD's FidelityFX API headers and signed `amd_fidelityfx_vk.dll` from SDK v1.1.4) is loaded at run time, not linked (see Temporal anti-aliasing, FSR).
- **Streamline 2.14.16** (`external/streamline`: NVIDIA's MIT headers committed, its signed DLLs with DLSS 310.9.16 fetched into `bin/` by `tools/fetch_streamline.py`) is loaded at run time too (see Temporal anti-aliasing, DLSS).

### Files

Files:

- `vulkan_device` (`VulkanDevice`):
  - `volkInitialize` once per process, since vk-bootstrap caches the loader's first functions for good and SDL unloads its own copy whenever video shuts down;
  - SDL's library loaded per device for `SDL_Vulkan_CreateSurface` and its instance extensions;
  - without a window (`nullptr`, the GPU tests) no SDL at all: vk-bootstrap's headless mode (no surface extensions), no surface, no present support asked of the GPU or checked on the queue;
  - Vulkan 1.3 with `VK_EXT_swapchain_colorspace` when present;
  - validation layers when `validation` and installed, errors counted, validation and performance warnings logged, the loader's own warnings dropped;
  - required features `dynamicRendering`, `synchronization2`, `samplerAnisotropy`, `depthClamp`, and for bindless textures `runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound`, `descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending`, and `bufferDeviceAddress` (required by Vulkan 1.3 itself);
  - optional ray queries (see Ray tracing) (`getBindlessTextureLimit`: the smallest update-after-bind sampler and sampled image limit);
  - `subgroupSizeControl` and `computeFullSubgroups` (required by Vulkan 1.3 itself), and for FSR (`supportsFsrFeatures`) `shaderFloat16`, `shaderInt16`, `shaderStorageImageReadWithoutFormat` and `WriteWithoutFormat`, asked for first and dropped only when no GPU has them, plus the extensions `VK_KHR_get_memory_requirements2` and `VK_KHR_dedicated_allocation` (core since 1.1, but FSR fetches `vkGetBufferMemoryRequirements2KHR` by its suffixed name, which a device returns only with them on; without them FSR crashed in its context creation);
  - `privateData` (required by Vulkan 1.3; Streamline keeps data on Vulkan objects), and `timelineSemaphore` and `descriptorIndexing` beside FSR's features; the optional extensions it is given (`OptionalExtensions`, DLSS's: `VK_KHR_push_descriptor`, `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_KHR_buffer_device_address`, all or none, `hasOptionalExtensions`; the instance's each when present);
  - one validation warning skipped: `rw_luma_history`, a bug in AMD's DLL (its shader declares the image `rgba8`, FSR creates it RGBA16F), so the engine's own messages still stand out;
  - discrete GPU preferred; one queue for drawing, copies and presenting; VMA;
  - helpers for semaphores, fences, samplers, command pools and SPIR-V modules.
- `owned_handle` (`OwnedHandle<T>`: one device object and its destroy function).
- `vulkan_check` (`vulkanCheck`: a `VkResult` other than success becomes an exception naming it).
- `vulkan_memory`: `VulkanBuffer` in `BufferMemory::Gpu`, `Upload` (mapped, sequential writes) or `Readback` (mapped, cached); `VulkanImage`, a 2D image with a view of all levels and its current `ImageState`.
- `image_state` (below).
- `immediate_commands` (`ImmediateCommands::run`: record, submit, wait; uploads, the atlas clear and the GPU tests' dispatches).
- `vulkan_compute` (`ComputePipeline`, see GPU tests and shader libraries).
- `vulkan_swapchain` with `swapchain_choice` (below), `vulkan_pipeline` (below).
- `frame_descriptors` (`FrameDescriptors`, `FrameUniforms`), `uniform_ring`.
- `bindless_textures` and `scene_layout` (GPU scene below).
- `render_pass` (`beginRendering`, `bindFragmentResources`, `drawFullScreenTriangle`).
- `vulkan_mesh`, `vulkan_texture`, `vulkan_bloom`, `vulkan_debug_lines`, `vulkan_overlay`, `vulkan_renderer`.

Outside the folder: `render/window_display` (`readWindowDisplay`, the window's HDR properties), `render/mesh_upload` (`checkMeshUpload`), `render/present_mode.h` and `render/gpu/column_major.h` (`writeColumnMajor`, matrices for GLSL).

### Kept from SDL_GPU

Kept from SDL_GPU, so the port drew the same pictures:

- **Descriptor layout.** The shaders of bloom, tone mapping, debug lines and the overlay keep SDL's descriptor layout, described per pipeline by `ShaderResources`, with set layouts shared through `DescriptorLayouts`:
  - set 0 vertex textures, unused;
  - set 1 vertex uniform blocks;
  - set 2 fragment textures as combined image samplers, then storage buffers;
  - set 3 fragment uniform blocks.
  - The scene's pipelines (main, shadow, tile reset) left it for the GPU scene's layout (`PipelineDescription::sharedLayout`, see GPU scene).
- **Pipelines.** Every pipeline has depth clamping on (SDL's `enable_depth_clip` defaults to false), counter-clockwise front faces, line width 1 and dynamic viewport and scissor.
  - `setViewport` flips the viewport (y at `y + height`, negative height) as SDL does, and `beginRendering` sets the whole target as viewport and scissor.
- **Formats:**
  - swapchain `B8G8R8A8_SRGB` (fallback `R8G8B8A8_SRGB`) in sRGB, or `R16G16B16A16_SFLOAT` in `EXTENDED_SRGB_LINEAR` for HDR;
  - captures `R8G8B8A8_SRGB` or `R16G16B16A16_SFLOAT`;
  - bloom levels `B10G11R11_UFLOAT_PACK32` when it can be drawn into, blended and filtered.
- **Samplers** are shared, not per texture:
  - surfaces trilinear, repeating, anisotropy 16 (or the GPU's limit), any level (`VK_LOD_CLAMP_NONE`; SDL set each texture's own level count, which the GPU clamps to anyway);
  - tables bilinear and clamped;
  - nearest (scene for tone mapping, the overlay);
  - linear clamped (bloom);
  - shadow compare (`GREATER_OR_EQUAL`, linear) and shadow raw depth (nearest).
- **Mipmaps** are blits from the level above with a linear filter, each level `max(1, size >> level)` (SDL let a non-square texture's short side reach 0 in its smallest levels), except a cut-out's, built on the CPU to keep its coverage (`ImageData::mips`) and copied in with level 0.

### Uniform blocks

Uniform blocks (`FrameUniforms`): SDL's push semantics, kept.

- `push(stage, slot, data)` copies the block into the frame's 4 MB upload buffer at the next offset aligned to `minUniformBufferOffsetAlignment`, and remembers it for the slot.
  - `UniformRing`: each slice leaves room for a whole 4 KB range, `maxUniformBlockBytes`, inside the buffer.
- `bind(commands, pipeline)` binds set 1 and set 3 (one descriptor set per stage and block count per buffer, every binding a `UNIFORM_BUFFER_DYNAMIC` with a 4 KB range) with the slots' latest offsets as dynamic offsets.
- A full buffer moves on to the next (made once, then reused every frame) and copies the slots' latest blocks along, so a draw's blocks are always in one buffer.
- `flush()` before submitting.
- Set 2 is written fresh per draw (`bindFragmentResources`) from `FrameDescriptors`: pools of 1024 sets (8192 textures, 1024 storage buffers, 16 uniform buffers for the scene set, 16 storage images for the probe updates) reset together each frame, another pool added when one runs out.

### Synchronisation

Synchronisation (`image_state`): an `ImageState` names a kind of use, and `imageStateUse` gives its stages, reads, writes and layout. The states:

- `Undefined`, `Acquired`, `ColourTarget`, `DepthTarget`, `FragmentRead`;
- `SurfaceRead`: sampled by fragment and compute shaders. Every uploaded texture ends its upload there, since the probe rays read the scene's textures in compute;
- `DepthFragmentRead`, `DepthComputeRead`;
- `ComputeRead` (sampled by compute);
- `ComputeStorage`: read and written in place by compute as a storage image, in the `GENERAL` layout (the probe atlases);
- `TransferSource`, `TransferDestination`, `Present`.

How the barriers work:

- `imageBarrier(image, aspect, from, to, ImageContents, levels)` waits for the old state's stages, flushes only its writes, makes the data visible to all of the new state's accesses, and changes the layout (from undefined when `ImageContents::Discard`).
- `needsBarrier` is false only for the same read-only state twice.
- `VulkanImage::transition` and `transitionImage` (for swapchain images) record the barrier and remember the state, which persists across frames, so a write after last frame's read still waits for it.
- `Acquired` starts from the colour output stage, the stage the frame's submission waits for the acquire semaphore at (`acquireWaitStage`), so the first barrier on a swapchain image chains after the semaphore.
- `Present` has no stage or access: the render-finished semaphore orders presentation.
- Tested in `image_state_test`.

### A frame

A frame (`render`) runs these steps:

1. `updateDisplayOutput`: window HDR properties every frame; `swapchain.supportsHdr()` only when HDR comes on; `chooseDisplayOutput`.
2. `prepareSwapchain`:
   - rebuild, after `waitIdle`, when missing, out of date, resized, HDR changed or the present mode changed;
   - HDR refused falls back to SDR and is marked unsupported;
   - the tone mapping, debug line and overlay pipelines remade for a new format;
   - false while minimised, after a 10 ms sleep.
3. `beginFrame`: wait for the frame's fence, reset its descriptor pools, uniform buffers and command pool, begin the command buffer.
4. Acquire (signals the frame's `imageAcquired`; out of date submits the empty buffer and draws nothing).
5. `ensureFrameTextures`: scene `R16G16B16A16_SFLOAT` and depth `D32_SFLOAT` at the swapchain's size, and the cluster masks buffer, remade after `waitIdle`.
6. `drawScene`.
7. Then the swapchain image `Acquired` to `ColourTarget` (discarded), one pass of tone mapping, debug lines and the overlay, to `Present`.
8. `submitFrame`: wait for `imageAcquired` at the colour output stage, signal the image's render-finished semaphore (one per swapchain image); the fence reset just before submitting; present.

### drawScene

`drawScene` starts on the CPU (camera, frustum, `prioritizeLights`, `packLighting`, `planShadows`), then records:

- debug line vertices and shadow tile data, written straight into the frame's upload buffers (no copy passes);
- the UI into the overlay;
- the ray tracing scene and the light probes' update (when the GPU has ray queries, `GlobalIlluminationSettings::enabled` and the frame has `ProbeCascades`; see Light probes);
- the clusters' light lists (when `RenderSettings::clusteredShading`, on by default, or `clusterHeatmap`; see Clustered shading);
- shadows;
- the depth prepass (when `RenderSettings::depthPrepass`, on by default);
- main pass;
- the probes' debug balls (when `showProbes`);
- the depth measurement for the sun's cascades (when `ShadowSettings::fitCascadesToDepth`; see Cascaded sun shadows);
- bloom.

**Shadows:** atlas `DepthFragmentRead` to `DepthTarget` (kept), one pass over the atlas loading and storing depth, per tile a viewport, the reset pipeline (`fullscreen_triangle.vert`, depth `ALWAYS`) and the casters, then back to `DepthFragmentRead`. The atlas is cleared to `gpuFarDepth` at creation.

**Depth prepass** (`drawDepthPrepass`, `render/gpu/depth_prepass`):

- depth to `DepthTarget` (discarded);
- a rendering block with no colour attachment that clears depth to far and stores it;
- `depthPrepassPipeline`: the main pass's `triangle.vert` and vertex layout with no fragment shader and no colour format; `GREATER`, written (`prepassDepthState`); `depthPrepassCutOutPipeline` the same with `cut_out.frag`, for cut-outs;
- and the same `drawBatches` as the main pass, so the depth image holds the nearest surface at every pixel.

**Main pass:** scene to `ColourTarget` (discarded, cleared to 0, 0, 0, 1) and depth to `DepthTarget`:

- After the prepass, depth is kept (the barrier from depth target to depth target makes the prepass's writes visible to this pass's tests) and loaded, with `mainAfterPrepassPipeline` (`EQUAL`, not written: `mainPassDepthState(true)`), so `triangle.frag` runs once per pixel whatever the draw order.
- Without it, depth is discarded and cleared to far, with `mainPipeline` (`GREATER`, written).
- The scene's two sets are bound once, one instanced draw per mesh (`drawBatches`), then the scene to `FragmentRead`.
- `EQUAL` needs both passes to compute each vertex's depth bit for bit the same: they share `triangle.vert`, whose `gl_Position` is declared `invariant`. GLSL promises identical results for an invariant output only; without it a compiler may, for instance, fuse a multiply and an add in one pipeline and not the other.
- `RenderStats::drawCalls` counts both passes' calls.
- In `triangle.frag` each light is skipped before its shadow lookups when `facesLight` says it is behind the surface (the same test `addLight` makes, on the normal-mapped normal, so the result is unchanged bit for bit).

**Measured:**

- Every golden image identical with the prepass on and off.
- Adding `invariant` changed 10 of the 12 references by rounding alone (separate multiplies and adds instead of fused ones; single pixels along silhouettes and shadow edges).
- GPU time, release, RTX 5080, mean of 200 captures (main pass plus prepass), in ms:

  | View | 1280x720 without prepass | 1280x720 with prepass | 2560x1440 without prepass | 2560x1440 with prepass |
  |---|---|---|---|---|
  | start | 0.092 | 0.093 | 0.259 | 0.254 |
  | gallery | 0.082 | 0.079 | 0.227 | 0.218 |
  | lamp field | 0.346 | 0.351 | 1.203 | 1.207 |
  | floodlights | 0.128 | 0.105 | 0.388 | 0.324 |

- The prepass itself costs 0.005 to 0.017 ms. It pays where surfaces hide each other (the floodlights view, 16%), not in the lamp field, whose cost was every pixel looping over all 64 point light seats (clustered shading's job, which took it to 0.75 ms; see Clustered shading).
- The skip of lights from behind saved about 3% of the lamp field's main pass at 1440p.

**After the main pass:**

- Bloom (`VulkanBloom`): the chain of `render/gpu/bloom` (see Light, HDR, tone mapping, bloom and shading), each step its level to `ColourTarget` (discarded when overwritten, kept when added onto), the filter, then `FragmentRead`.
- Debug lines (`VulkanDebugLines`): one growing upload vertex buffer per frame in flight.
- Overlay (`VulkanOverlay`): ImGui's changed textures uploaded first (`ImGui_ImplVulkan_UpdateTexture`, which submits and waits on its own), then the UI drawn into the `R8G8B8A8_UNORM` overlay (cleared to transparent) with `ImGui_ImplVulkan_RenderDrawData`. `startDebugUi` gives ImGui the device, queue, dynamic rendering with that format, and two frames' buffers.
- On an error mid-frame `abandonFrame` ends an open pass, moves an acquired image to `Present`, submits and presents, and the shadow cache is forgotten.

### Capture and frames in flight

`capture` draws `drawScene` and the tone mapping pass into a capture image (its own tone mapping pipeline for that format) at the golden size, copies the scene and final images into one read-back buffer, adds a transfer-to-host barrier, submits, waits for the GPU and reads both back. The display output is restored afterwards.

Frames in flight: `EngineSettings::framesInFlight` (default 2, 1 to `maxFramesInFlight` = 2, `validFramesInFlight` clamps; `render/frames_in_flight.h`).

- Given to the constructor and applied live by `Application::applyPresentation` through `setFramesInFlight`, which:
  - waits for the GPU;
  - calls `createFrames` anew and `VulkanDebugLines::setFramesInFlight`;
  - and has the swapchain rebuilt by `prepareSwapchain` because `swapchainFramesInFlight` differs.
  - ImGui's backend keeps `maxFramesInFlight` vertex buffer sets throughout.
- Each frame has its own command pool and buffer, fence (created signalled), acquire semaphore, descriptors, uniforms, shadow tile buffer and timestamp query pool.
- The swapchain has at least one image per frame in flight, the surface's minimum, and three for Mailbox (`chooseSwapchainImageCount`, tested in `swapchain_choice_test` with the format, present mode and extent choices).

### GPU timing and debug labels

GPU timing and debug labels: `render/gpu/gpu_timing` (no graphics API, tested in `gpu_timing_test`) lists the timed passes in recording order as `GpuPass`:

| Pass | What it times |
|---|---|
| `DebugUi` | the UI drawn into its overlay image |
| `RayScene` | the ray tracing scene's top-level structure, only while something traces rays |
| `Atmosphere` | SKY: the sky's tables and flat ambient, only with an atmosphere |
| `Probes` | the light probes' trace and update |
| `Clusters` | the clusters' light lists, a compute pass |
| `Shadows` | |
| `DepthPrepass` | |
| `MirroredLights` | MIRROR: light reflected off mirrors onto what the prepass drew, only while the pass runs |
| `Main` | |
| `ProbeBalls` | the probes' debug balls |
| `RayDebug` | the ray tracing debug view |
| `DepthRange` | the screen's depths measured for the sun's cascades, a compute pass |
| `EyeAdaptation` | EYE: the scene's brightness and the adapted exposure, two compute passes, while it adapts |
| `Bloom`, `ToneMap`, `DebugLines` | |
| `Overlay` | the composite |

The same file has:

- `gpuPassName`, giving the readout's short names UI, TLAS, SKY, PROBES, CLUSTERS, SHADOW, PREPASS, MIRROR, MAIN, BALLS, RAYS, DEPTHS, EYE, BLOOM, TONE, LINES, OVERLAY, also used as the debug labels;
- `gpuTimestampCount` (one stamp before the first pass and one after each);
- `timestampTicksBetween` (the difference masked to the queue's `timestampValidBits`, so a counter that rolls over still gives the right count);
- and `gpuTimingsFromTicks` (seconds per pass from the device's `timestampPeriod`; the passes add up to `totalSeconds`; not `valid` with zero valid bits).

**Recording the stamps:**

- Each frame in flight has a timestamp query pool of `gpuTimestampCount` (none when the queue's `timestampValidBits` is 0), reset with `vkCmdResetQueryPool` at the start of its command buffer.
- `beginGpuPass`/`endGpuPass` wrap each pass:
  - `writeTimestampsThrough` writes every stamp up to the pass's start or end with `vkCmdWriteTimestamp2` at `ALL_COMMANDS`, so a stamp lands when everything before it has finished, and a pass skipped this frame gets two stamps at the same moment, lasting 0;
  - a debug label (`VulkanDevice::beginLabel`/`endLabel`) marks the pass in between;
  - the tone mapping, debug line and overlay labels sit inside their one rendering block.
- `finishGpuTimings` writes any stamps left and marks the frame complete (`render` and `capture` only, so an abandoned frame or one without a swapchain image is never read).

**Reading them back:**

- `beginFrame` reads a complete frame's stamps after its fence with `vkGetQueryPoolResults` (64-bit, no wait; `VK_NOT_READY` skips the frame) into `unreportedGpuTimings`.
- `drawScene` moves them into `RenderStats::gpu`, so each measured frame is reported once, as many frames late as there are in flight.
- `Application` records them into its `gpuProfiler` (`makeGpuProfiler`: GPU (the total) as section 0, then one section per pass; `recordGpuTimings` skips invalid timings), updated with the CPU profiler and drawn on readout line `gpuProfileReadoutLine` (9) in the F4 view.

**Debug names:**

- `VulkanDevice` turns on `VK_EXT_debug_utils` whenever the instance has it (vk-bootstrap already adds it for validation messages), in release builds too, since labels and names cost nothing without a tool attached.
- `nameObject` names objects, so RenderDoc, Nsight and validation messages show names instead of handles.
  - `ImageDescription::name`, a string literal, names each `VulkanImage`: Scene colour, Scene depth, Shadow atlas, Bloom level, Debug UI overlay, Golden capture, Probe irradiance atlas, Probe distance atlas.
  - The frame command buffers are Frame commands.

## GPU scene

The scene's two passes (main and shadows) read everything from storage buffers written once per frame, so nothing is bound or pushed per object and every object with the same mesh is one instanced draw in each view (phase 6c, `gpu-scene`).

**Files and tests:** the API-free parts are in `render/gpu/gpu_scene` (tested in `gpu_scene_test`), `light_uniforms`, `material_uniforms` and `shadow_map`; the Vulkan parts in `render/vulkan/bindless_textures` and `scene_layout`.

### Records

Records (std430, mirrored byte for byte in the shaders, each with a `static_assert` on its size):

- `GpuInstance` (160 bytes: world model matrix, normal matrix, handedness, material number, mesh number (for ray hits); `packInstance(draw, material)`).
  - One per `DrawItem` of the frame, instance i being draw i, the same for every view (ray tracing instances and GPU culling can read the same records later).
- `GpuMaterial` (96 bytes, see Assets and materials), deduplicated per frame by `MaterialTable::add` (an FNV-1a hash of the packed bytes, then a full comparison; packing has no padding).
- Lights, in `GpuLights` from `packLighting` with `ambient` and `counts`, shadows filled in by `attachShadows`:
  - `GpuDirectionalLight` (48: direction towards the light, radiance, `shadowTile` (its first cascade), `cascadeCount`, `tanAngularRadius`);
  - `GpuPointLight` (48: position and range, radiance and source radius, `firstShadowTile`, `shadowStrength`, `emitterRadius`);
  - `GpuSpotLight` (64).
  - The buffers hold `maxPointLights` (1024) point, `maxSpotLights` (256) spot and `maxDirectionalLights` (4) directional lights, all seated by default since clustered shading (each pixel loops only over its cluster's lights).
- `ShadowTileData` as before.
- A draw list of `uint32` instance numbers.

`GpuFrameData` (336 bytes, std140 uniform buffer, vertex, fragment and compute) holds:

- the camera-relative view-projection, the camera position, ambient, counts, the atlas texel size;
- the cluster grid (`clusterTiles`, `clusterScreen`, `clusterSlicing`, `viewRight`, `viewUp`, `viewForward`, laid out as `clusters.glsl`'s `ClusterGrid`);
- `shadingOptions` (x clustered shading, y the heatmap) and `cascadeOptions` (x the cascades' blend band, y 1 to dither between them);
- then the light probes' numbers (`probeOrigin`, `probeSpacing`, `probeAtlas`, `probeLights`, zero without probes; `packProbeSampling`, see Light probes);
- `textureOptions`: x the surface textures' mip bias (`textureLodBias`, see Temporal anti-aliasing, Upscaling), passed to `triangle.frag`'s three `texture()` reads;
- `mirroredLightOptions`: x 1 while the main pass adds the mirrored light pass's images, y the frame (the TAA frame mod 1024), turning rough mirrors' samples (see Mirrored lights);
- and `ambientOcclusionOptions`: x 1 while the main pass reads ambient occlusion, y 1 with multiple bounces, z 1 with specular occlusion (see Ambient occlusion).
- It is made by `makeFrameData(lights, relativeViewProjection, cameraOrigin, clusters, clusteredShading, clusterHeatmap, shadows)`; the atlas texel size comes from `shadowAtlasSizeFor(shadows)`.

`GpuShadowView` (64 bytes): a tile's world view-projection, the scene layout's one push constant (vertex stage).

### Batching

- Batching: `batchByMesh(draws, chosen, firstInstance)` stable-sorts the chosen draw indices by how they are drawn (plain, double-sided, cut-out, double-sided cut-out) and then by mesh (each mesh's draws keep their order, so a frame always gives the same draw list) and cuts the runs into `DrawBatch`es {mesh, firstInstance, instanceCount, cutOut, doubleSided}.
- `drawBatches(commands, batches, BatchPipelines{plain, cutOut, cull})` binds the pass's pipeline for each batch's style when it changes (cut-outs last, so once) and sets the cull mode when it changes (`vkCmdSetCullMode`: the scene's pipelines have `PipelineDescription::dynamicCullMode`), none for double-sided batches.
- `drawBatches` binds each mesh's buffers and calls `vkCmdDrawIndexed(indexCount, instanceCount, 0, 0, firstInstance)`, so `gl_InstanceIndex` is the draw list entry.
- The draw list is the main pass's visible draws in batch order (entries from 0), then each redrawn shadow tile's casters in batch order (`prepareShadowDraws`, entries from the main pass's count).

**Measured** (lamp field with every tile redrawn; release, RTX 5080, measured around golden captures):

- 161 visible draws in 10 calls, 1861 caster instances in 783 calls;
- about 61 KB uploaded per frame;
- CPU recording 1.75 ms before the GPU scene, about 1.15 ms after.

### Shaders

- Declaration-only includes:
  - `instances.glsl` (the `InstanceData` struct, `Instances` at set 1 binding 1 and `DrawList` at binding 10);
  - `frame_data.glsl` (the `FrameData` block, binding 0);
  - `materials.glsl` (the bindless texture array and the `MaterialData` buffer, shared by `triangle.frag` and `ray_debug.frag`);
  - `scene_lights.glsl` (the light records and their buffers, bindings 3 to 5, shared by `triangle.frag` and `cluster_lights.comp`).
- `frame_clusters.glsl` builds the frame's `ClusterGrid` from the block (`frameClusterGrid`).
- `triangle.vert` reads `instances[drawList[gl_InstanceIndex]]`, subtracts `cameraOrigin` from the model matrix's translation (exactly `relativeTo`), and passes the material number flat to `triangle.frag`.
- `triangle.frag` reads `materials[materialIndex]` and its textures as `textures[nonuniformEXT(slot)]` (neighbouring pixels of one shading group may be different objects; `GL_EXT_nonuniform_qualifier`), and each light's shadow from its record.
- `shadow.vert` computes `tileMatrix * (model * position)` in world space, so cached tiles stay keyed on world matrices.

### Descriptors

Descriptors (`scene_layout`):

**Set 0** is `BindlessTextures`:

- one binding of 16384 combined image samplers, or `getBindlessTextureLimit` if lower, all with the surface sampler;
- layout and pool update-after-bind, binding partially bound and update-unused-while-pending, so `uploadTexture` writes slot handle + 1 while frames using other slots are in flight;
- the white stand-in in slot 0 at start-up;
- `checkTexturesUploaded` throws for a material texture never uploaded.

**Set 1** is the frame's scene (`SceneBinding`), written once per frame by `writeSceneSet` from the frame's `FrameDescriptors` pools:

| Binding | Holds |
|---|---|
| 0 | `FrameData` uniform |
| 1 | instances |
| 2 | materials |
| 3 to 5 | directional, point and spot lights |
| 6 | shadow tiles |
| 7 | the atlas with the comparison sampler |
| 8 | the atlas raw |
| 9 | the BRDF table |
| 10 | the draw list |
| 11 | the cluster masks |
| 12 and 13 | the light probes' irradiance and distance atlases (fragment and compute) |
| 14 | the lights probe rays' hits may need (compute) |

- The frame data and the point and spot lights are visible to the compute stage too, and the masks to fragment and compute.

**Sharing the layout.** `SceneLayout` owns set 1's layout and the pipeline layout of both sets plus the push constant, shared by the main, shadow and tile reset pipelines (`PipelineDescription::sharedLayout`) and the cluster pass (`ComputePipelineDescription::sharedLayout`).

- So `bindScene` (graphics bind point by default; compute for the cluster pass, whose bound sets are kept apart) binds both sets once per pass and they stay bound across those pipelines.
- The debug UI's pass binds sets of its own before the shadow pass, hence once per pass rather than once per frame.

**Buffers.** Each frame in flight has its own `SceneBuffers`: `UploadArray`s in upload memory (frame data, instances, materials, three light buffers, shadow tiles, draw list).

- Each is grown (to twice its size, or what is needed) when a frame needs more, safe because the frame's fence has passed.
- `writeScene` fills them before any pass is recorded: `drawScene` first settles every view's draws (`visibleDraws` and `batchByMesh` for the camera, `prepareShadowDraws` for the tiles).

## Clustered shading

Each pixel of the main pass loops only over the lights that may reach its cluster, instead of every light in the frame (phase 6c, `clustered-shading`).

**Files and tests:** the maths is in `render/gpu/clusters` (API-free, tested in `clusters_test`) and `clusters.glsl` (mirrored, GPU-tested in `tests/gpu/clusters_gpu_test`); its numbers in `render/gpu/clusters_shared.h`.

### The grid

The grid: `ClusterGrid` (`makeClusterGrid(camera, screenWidth, screenHeight)`, throws for an empty screen) cuts the view into clusters.

- **Tiles:** square tiles of `clusterTilePixels` (64), the last column and row cut off by the screen's edge: 2560x1440 is 40 x 23 tiles.
- **Slices:** each tile is cut into `clusterDepthSlices` (24) depth slices spaced exponentially, slice k starting at

  near * (far / near)^(k / 24)

  (`sliceStartDepth`; each about 1.33 times deeper than the last for near 0.1 and far 100).
- **Coordinates:** positions are in cluster view space: x right, y up, z the depth along the view (positive in front), measured from the camera (`toClusterView`, with the view matrix's rows as `right`, `up`, `forward`).
- **A depth's slice** is `floor(log(depth) * sliceScale + sliceBias)`, clamped to the grid (`clusterSlice`; 0 for depths at or behind the camera).
- **Numbering:** clusters are numbered along a row, then down the screen, then slice by slice (`clusterIndex`). A pixel (from the top-left corner, like `gl_FragCoord`; the viewport flips y) finds its cluster with `clusterOfPixel`.
- **Bounds:** `clusterBounds` is the box around a cluster's piece of the frustum (its eight corners; slice 0 starts at the camera), grown on every side by `clusterBoundsMargin` (0.001) times its far depth, so a pixel whose depth or tile rounds into a neighbour still finds every light that reaches it.
- Tests: the grid against the camera's own projection, the slices' spacing, the bounds.

### The lists

The lists: one bit per light per cluster.

- `clusterWords` (40) words per cluster: the point lights' `clusterPointWords` (32, for `maxPointLights`) then the spotlights' `clusterSpotWords` (8, for `maxSpotLights`; both checked by `static_assert`).
- 160 bytes per cluster (`clusterMaskBytes`: about 3.5 MB at 2560x1440).
- A light's bit is set where its range sphere touches the cluster's box (`sphereTouchesBox`).
- For a spotlight, its cone (outer angle, range) must also touch the box's bounding sphere (`coneTouchesSphere`, Wronski 2016 "Cull that cone"): the signed distance from the cone's slanted edge, `cos * across - sin * along`, then past the range or behind the apex. Cones of 90 degrees or wider are only range tested.
- Both tests are conservative.
- `buildClusterMasks(grid, lights)` is the CPU mirror of the whole pass. The tests check:
  - the property everything rests on, that every light reaching a point is listed in that point's cluster (random lights around random points all over the view);
  - and that lights are listed only near where they reach.

### The pass

The pass: `cluster_lights.comp` (`GpuPass::Clusters`, CLUSTERS in F4; `clusterLightsPipeline`, a `ComputePipeline` on the scene's pipeline layout), one invocation per cluster in groups of `clusterGroupSize` (32), dispatched with `workgroupCount`.

- **Lights are taken one word (32) at a time:**
  1. each invocation of a group turns one light into cluster view space into shared memory;
  2. a `barrier()`;
  3. then every invocation tests its cluster against all 32 and writes its own word;
  4. another `barrier()` before the next word.
- Each light is read from its buffer once per group, no two invocations write the same word, and the result does not depend on which finishes first.
- Only words for the frame's light counts are written, and only those are read.
- It reads the frame's `FrameData` (the grid) and light buffers through set 1 and writes binding 11, `clusterMasks`: one `VulkanBuffer` in GPU memory (`BufferMemory::Gpu`), sized with the frame in `ensureFrameTextures`, shared by the frames in flight like the frame images.
- `buildClusterLists` records it after the debug UI and before the shadows, bound at the compute bind point, between two buffer barriers (`bufferBarrier(commands, buffer, before, after)` with `BufferUse{stages, access}`, `render/vulkan/buffer_barrier`):
  - fragment shader (no access: a write after a read needs only the order) to compute shader storage writes;
  - then compute storage writes to fragment shader storage reads.
- It runs when `RenderSettings::clusteredShading` (on, switchable live) or `clusterHeatmap` is on.

### Shading

Shading: `triangle.frag`, when `shadingOptions.x`, finds its cluster's first word (`firstClusterWord`), and for each word up to the frame's counts walks the set bits lowest first (`findLSB`, then clearing it), calling `addPointLight` or `addSpotLight` (the per-light shading, unchanged, shared with the plain loops used when clustering is off).

- Lowest bit first is slot order, and every light that reaches the pixel is listed, so the pixel adds exactly the lights it added before, in the same order: all twelve golden images were identical bit for bit with clustering on and off.
- Each word is first OR-ed across the wave (`lightsOfTheWave`, `subgroupOr`, `GL_KHR_shader_subgroup_arithmetic`), so the pixels shaded together walk one list and read the same light records at once. A light from a neighbour's list cannot reach this pixel (it would be in its own list), so it adds nothing.
- `checkFormats` throws if the GPU lacks subgroup arithmetic in fragment shaders.

**The heatmap** (`RenderSettings::clusterHeatmap`, off, a debug view; `shadingOptions.y`) mixes three quarters of `clusterHeatColour(clusterLightCount())` over the pixel: black for no lights, then blue, green, yellow and red at `clusterHeatmapLights` (16) and above (`clusterHeatRamp`, branch-free clamped lines).

- Seen on the golden views: the lamp field's near ground lists about 8 to 12 lamps per cluster and its far rows 16 or more, while only 4 to 8 reach any point (far slices are deep and grazing tiles cover a lot of ground).
- Each extra listed light costs a pixel one distance check.

### Measured

Measured (release, RTX 5080, 2560x1440, mean of 100 captures, main pass), in ms:

| View | Looped | Clustered |
|---|---|---|
| lamp field | 1.18 | 0.75 |
| lamp field, point light shadows off | 0.62 | 0.30 |
| start | 0.22 | 0.20 |
| gallery | 0.19 | 0.17 |
| floodlights | 0.30 | 0.30 |

- With point light shadows off, what remains is the soft shadows of the lamps that do reach.
- The cluster pass costs 0.004 to 0.008 ms.
- Walking each pixel's own list instead of the wave's union measured the same at today's light counts.
- With every light seated (`defaultPointSeats = maxPointLights`, `defaultSpotSeats = maxSpotLights`), the lamp field draws all 88 lamps in view in 0.78 ms (the old loop: 1.40).

## Cascaded sun shadows

The brightest directional light (slot 0 after `prioritizeLights` ranks them by brightness; `maxShadowedDirectionalLights` = 1) gets `ShadowSettings::cascadeCount` (4, 1 to `maxShadowCascades`) box views, finest first, as tiles in a row from its `LightShadow::firstTile`.

- `LightShadow::cascades` says how many.
- `emitterRadius` holds tan of `DirectionalLight::angularRadius`, 0.00465 rad, the sun's.

**Files and tests:** `render/gpu/cascades` (mirrored by `assets/shaders/cascades.glsl`, tested in `cascades_test` and `tests/gpu/cascades_gpu_test`).

### Splits

Splits: `cascadeSplits(near, far, count, lambda)` from the camera's near plane to `min(ShadowSettings::directionalDistance, far plane, fitted distance)` (200 m unfitted; see Fitted to the screen below).

- The practical scheme, with `cascadeSplitLambda` 0.95:

  `lambda × logarithmic + (1 − lambda) × even`

- About 3, 9, 36 and 200 m for a 70° 16:9 view.
- The ends set exactly.

### Fit

- Fit: `cascadeSphere(camera, near, far)`, the slice's smallest bounding sphere, on the view axis at

  `(f + n)(1 + t²) / 2`

  (t² the sum of the squared half-width and half-height tangents), or at the far end's centre with radius `f t` when that is nearer.
  - Its radius depends only on the lens, so it is identical however the camera turns (tested with `==`).
- `cascadeBox(direction, sphere, casterReach, tileSize)`: a box looking along the light (`shadowViewUp`, shared with the spot and point views), its centre snapped to whole texels across the light, reaching `directionalCasterReach` (200) further towards the light.
  - Returns the GPU matrix, `depthRange` (2 r + reach) and `texelSize` (2 r / tile size).

### Tiles

- Tiles: `cascadeTilePixels(ShadowSettings::cascadeResolution)`, 1024, 2048 or 4096 (default `Pixels4096`, the High and Ultra presets).
- `shadowAtlasSizeFor(settings)` is the usual 8192 x 4096 atlas (squares of up to 2048) unless the cascades are 4096, when it grows by their 4 x 4096² texels to 8192 x 12288 (384 MB, squares of up to 4096; `ShadowAtlasSize` in `render/gpu/shadow_atlas`, which also holds the tile size constants).
- `planShadows` throws `std::invalid_argument` when its layout is for another size.
- `VulkanRenderer::ensureShadowAtlas`, called in `drawScene` before planning (so golden captures get it too), waits for the GPU, remakes the atlas image, the layout and the tile memory when the size changes, and the next set 1 points at the new image.
- Each cascade's `ShadowTileData`:
  - the box times `translation(camera)` for the shader;
  - `offset.y` = `normalOffsetTexels` × texel size;
  - `lens` = (0, depth range, texel size, radius); a zero `lens.x` tells boxes from perspective tiles.
- Cached like every tile: a box that did not move (the camera moved under a texel) and whose casters did not change is not redrawn.

### Lookup

Lookup (`sunShadow` in `triangle.frag`), for each cascade, finest first:

1. The point is moved towards the sun by `sunShadowBias(offset.y, n·l)` = base × (1 + min(tan θ, `MAX_SUN_BIAS_SLOPE` = 10), in `shader_shared.h`).
2. Then `cascadeEdgeDistance(projected, depthRange / radius)`: 1 at the centre, 0 at any face of the box, in radii (across, and from the far and near sides).
3. The first cascade with a positive edge distance is used.

Blending between cascades:

- `cascadeBlendWeight(edge, band)` (`cascadeBlendBand` 0.1 of the radius; 0 switches at once) mixes in the next cascade near the edge, or full light past the last one. A point the next box misses stays with this one.
- `CascadeTransition::Dither` instead keeps one cascade per pixel (`cascadeDitherKeeps(weight, interleaved gradient noise)`), one lookup instead of two.
- Blend band and transition reach the shader as `GpuFrameData::cascadeOptions`.

**Why the bias moves along the light:** an offset along the normal moves the point sideways as the sun sees it, by an amount that grows with the texel, so cascades of different texel sizes found a shadow's edge in different places (a visible step at the seam under a low sun). Moving along the rays changes only the depth in every cascade (`testMovingTowardsTheSunKeepsThePlaceInEveryCascade`). It also removed the stair-stepped edge of the sunlit room's window patch.

### Soft edges

Soft edges: `softShadowFromTile` handles boxes too:

- distances from the box's near side (`boxShadowDistance(depth, depthRange)`);
- blocker search `sunBlockerSearchTexels(tan, receiver, texel)` (a blocker at the near side);
- penumbra `sunPenumbraTexels(tan, receiver, blocker, texel)` = tan × (receiver − blocker) in the cascade's texels, clamped to 1-12 like the lamps'.
- The width is worked out in metres and converted per cascade, so it matches across a seam where the coarser cascade can show it.

### Measured texels

Measured texels for a 70° 16:9 view, 4096 (2048) tiles, unfitted:

| Cascade | Texel, 4096 tiles | Texel, 2048 tiles |
|---|---|---|
| first cascade | 0.22 cm | (0.44) cm |
| to 9 m | 0.65 cm | (1.3) cm |
| to 36 m | 2.5 cm | (5) cm |
| to 200 m | 14 cm | (28) cm |

A cascade's box is about 2.9 times its far distance across, so fitting the far end to the screen shrinks every one.

### Fitted to the screen

Fitted to the screen (sample distribution shadow maps, Lauritzen 2011; `ShadowSettings::fitCascadesToDepth`, on): the cascades end at the farthest surface on screen instead of the shadow distance.

- Only the far end is fitted: starting the splits at the nearest surface pushes every split outwards and makes the nearest surfaces coarser.

**The measurement** (`render/gpu/depth_range`, mirrored by `depth_range.glsl`; numbers in `depth_range_shared.h`):

- `depth_range.comp` (`GpuPass::DepthRange`, DEPTHS in F4) reads the depth image after the main pass.
  - The main pass then stores its depth (`drawMainPass(..., keepDepth, ...)`); the depth image is `SAMPLED` too and goes to `ImageState::DepthComputeRead`.
- Each invocation reads a 2x2 square of pixels in 16x16 groups, keeping `ScreenDepthRange` {farthest bits, nearest bits}.
  - Reversed-Z, so the farthest surface has the smallest depth, and the far depth itself (0) is sky and skipped (`isSurfaceDepth`).
  - Positive floats' bits sort like the numbers, so `subgroupMin`/`subgroupMax` across the wave, shared-memory atomics per group and two global atomics per group combine them in any order.
- `VulkanDepthRange` (`render/vulkan/vulkan_depth_range`) owns:
  - the pipeline (`ComputePipelineDescription::sampledImages`: the depth at binding 1, the range buffer at 0);
  - the range in GPU memory (reset by `vkCmdUpdateBuffer`, then copied);
  - and a read-back buffer per frame in flight.
- `measure` records it; `collect(frameIndex)` reads it once after that frame's fence (`beginFrame`), so the CPU uses a measurement a frame or two late (`unusedDepthRange`).
- Depth to distance: `farthestSurfaceDistance` (`linearShadowDepth` with the camera's planes).

**The fit** (`cascades.h`):

- `ShadowDistanceFit {optional step}`: the distance is `shadowDistanceStep` (1.25) to the step's power in metres from 1 m.
- `fitShadowDistance(previous, farthest)` takes the step reaching `farthest × shadowDistanceMargin` (1.1, for the frames it is late).
  - It climbs at once but comes down only once the need is `shadowDistanceShrinkSteps` (2) steps lower, so the boxes keep one size (and stay cached) while the view moves a little.
  - No surface (only sky) leaves it unfitted.
- `fittedShadowDistance` (infinity unfitted) goes to `planShadows(..., fittedSunDistance)`.
- `RenderStats::sunShadowDistance` shows where the cascades end (SUN TO on the F4 tile line).
- Golden captures first draw the frame once to measure it (`measureDepthsBeforeCapture`), then restore the light history, atlas layout and shadow cache, so a capture is still a first frame plus its measurement.

**Measured (RTX 5080):**

- The sunlit room fits to 11.6 m, texels 0.032, 0.092, 0.27 and 0.81 cm (from 0.22, 0.65, 2.5 and 14).
- Views reaching the horizon stay at 200 m.
- The pass costs 0.015-0.04 ms at 2560x1440.
- The main pass grew 0.06 ms in the demo's start view and 0.09 ms in the sunlit room, since a penumbra once under one texel now spans several and its samples spread wider in the atlas.

## Ray tracing

Hardware ray tracing through ray queries (phase 6d, `ray-queries`): any shader asks the GPU what a ray hits first and gets the distance, the instance, the triangle, its barycentric coordinates and whether it struck the front or the back.

**Optional:** `VulkanDevice` turns on `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and `VK_KHR_deferred_host_operations` with their features when the GPU has them (`supportsRayQueries`, `getScratchAlignment`). Without them `VulkanRenderer::rayTracing` stays null and everything ray-traced is off.

**Buffer device addresses** (a buffer's place in GPU memory, which shaders follow like a pointer and structures are built from) are required, as Vulkan 1.3 itself requires them:

- `bufferDeviceAddress`, VMA's `BUFFER_DEVICE_ADDRESS` flag, `VulkanBuffer::getDeviceAddress`, `UploadArray::getDeviceAddress`.
- Every `VulkanMesh` buffer has `SHADER_DEVICE_ADDRESS` usage (and, with ray tracing, `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY`), `getVertexAddress`, `getIndexAddress`, `getVertexCount`.
- Its upload barrier also makes the data visible to fragment and compute storage reads and, with ray tracing, to structure builds.

### The API-free part

API-free (`render/gpu/ray_tracing`, numbers in `ray_tracing_shared.h`, tested in `ray_tracing_test`):

- **Masks:**
  - `solidRayMask` (1, shadow casters: they block light);
  - `visualOnlyRayMask` (2, the rest: the glowing faces of lights already counted directly, the sun disc, the moon, bulbs);
  - `everythingRayMask`, `rayMaskFor(draw)`.
- **`GpuRayInstance`** (64 bytes, `VkAccelerationStructureInstanceKHR`'s layout, checked by `static_assert` in `vulkan_ray_tracing.cpp`) from `packRayInstance(draw, instanceNumber, meshStructure, flags)`. It holds:
  - the model's top three rows row-major;
  - the instance number (24 bits, `maxRayInstances`) under the mask;
  - the record offset under the flags;
  - the mesh structure's address.
- **`hitsFrontFace(a, b, c, direction, handedness)`:** the front is the side the corners turn anticlockwise when seen from. The GPU decides it in the instance's own coordinates, so a mirrored instance's outside stays its front; no instance flags needed, checked against the GPU.
- **The debug view's camera:** `RayDebugView` (Off, Normals, Albedo, Faces), `GpuRayDebugView` (80 bytes of push constants: right and up scaled by the tangents of half the view, forward, the camera's position with the view in w, the screen's size) from `packRayDebugView`.
- `cameraRayDirection` and `hitCornerWeights` (mirrored by `assets/shaders/ray_tracing.glsl`).
- `GpuInstance::material[1]` holds the draw's mesh number, so a hit finds its triangle's corners.

### Acceleration structures

`VulkanRayTracing` (`render/vulkan/vulkan_ray_tracing`) keeps one bottom-level structure per mesh, built at upload by `addMesh`, called by `VulkanRenderer::uploadMesh`:

- opaque triangles from the vertex and index buffers by address, 48-byte stride, `PREFER_FAST_TRACE | ALLOW_COMPACTION`;
- the compacted size read through a `ACCELERATION_STRUCTURE_COMPACTED_SIZE` query once the build is done, then copied into a buffer of that size with `COMPACT` and the original dropped;
- a barrier makes it readable by top-level builds and ray queries in later submissions.

Scratch memory is allocated one alignment larger and its address rounded up (`createScratch`, `scratchAddress`).

**The top level.** Each frame in flight has its own top-level structure, scratch, instance and mesh table `UploadArray`s and descriptor set (from a pool of exactly that many).

- `packInstances(draws)`: instance i is draw i; throws for a mesh without a structure.
- `buildScene(commands, frameIndex, instances)`:
  - writes the mesh table and the instances;
  - grows the structure (from 256 instances, doubling) when needed;
  - rebuilds it from scratch every frame (simple, never stale; 0.05-0.11 ms for 29-900 instances on the RTX 5080);
  - barriers it to fragment and compute ray queries;
  - and rewrites the frame's set: binding 0 the structure, binding 1 the mesh table (`{vertices, indices}` addresses per mesh).

**`ray_scene.glsl`** declares:

- that set (set 2 by default, `RAY_SCENE_SET`);
- the buffers by address (`GL_EXT_buffer_reference_uvec2`: addresses as two 32-bit halves, so no 64-bit integers are needed; vertices read as a float array of `MESH_VERTEX_FLOATS` per vertex, since std430 would pad a struct of vec3s);
- and `meshSurfaceAt(mesh, triangle, barycentrics)` (the interpolated normal and texture coordinates in the mesh's own space).

### The debug view

The frame's structure is built after the debug UI while something traces rays: the light probes (see Light probes) or the debug view.

The debug view (`RenderSettings::rayDebugView`, F5 Passes; Off by default):

- While it is on, the renderer builds the frame's structure after the debug UI (`GpuPass::RayScene`, TLAS in F4).
- After the main pass, it draws `ray_debug.frag` over the scene image (`GpuPass::RayDebug`, RAYS; `drawRayDebugView`, the scene colour discarded and redrawn).
  - Its layout is the scene's two sets plus the ray tracing set and the camera as push constants (`rayDebugLayout`).
- One ray per pixel against every mask bit shows:
  - the hit's world normal (through the instance's normal matrix);
  - its albedo (`materials.glsl`, the colour texture's full-size level times the base colour);
  - or its facing (front green, back red, shaded by the angle);
  - misses dark grey.
- The scene layout's instance binding is visible to fragment and compute shaders for it (and materials to compute), for DDGI later.
- **Measured** at 2560x1440: 0.09-0.28 ms for one primary ray per pixel.

## Light probes (DDGI)

Global illumination's light probes (phase 6d, `ddgi-probes`, `ddgi-sampling`, `ddgi-fast-lights`, `ddgi-scene-events`; the method, its formulas and the papers' numbers are in `docs/ILLUMINATION.md`). This section covers:

- the probe grid;
- its three atlases (irradiance from still lights, irradiance from fast lights, distances);
- the rays and the update;
- sampling at surfaces (the main pass and the probe rays' hits, so light bounces once more each frame);
- point and spot lights at hits;
- and a debug view.

Frames without probes draw exactly as before (the demo's golden images are unchanged), and so do frames without fast lights (the fast atlas stays exactly zero).

### Placing probes

Placing probes (`ddgi-cascades`; `render/gpu/probe_cascades`, numbers in `ddgi_shared.h`, mirrored by `assets/shaders/probe_cascades.glsl`, tested in `probe_cascades_test` and `ddgi_gpu_test`'s cascades): a game places one `ProbeCascades` component, and the renderer keeps the probes in nested grids round it, so no level places or tunes probes.

- `ProbeCascades` (`scene/probe_volume.h`): `followCamera`, true; `centre`, followed otherwise.
- It is gathered by `collectLighting` into `FrameLighting::probeCascades` (the first).

**How many and how dense** is the settings' choice: `probeCascadeLayout(settings)`.

| Setting | Default | Meaning |
|---|---|---|
| `probeCascades` | 6 (4 before `ddgi-far-cascades`) | how many cascades |
| `probeSpacing` | 0.5 m | spacing of the densest |
| `densestProbesAcross` | 31 | the densest cascade's probes along both level axes |
| `densestProbesHigh` | 17 | the densest cascade's probes up |
| `cascadeProbesAcross` | 21 | every other cascade's probes along both level axes |
| `cascadeProbesHigh` | 13 | every other cascade's probes up |

- At least 2 a side, the up axis giving way first until a cascade's layer fits the atlas width and one dispatch, then as many cascades as fit, at most `DDGI_MAX_CASCADES` 6.
- Each cascade has twice the last's spacing (`probeCascadeSpacing`).

`probeCascadesAround(centre, layout, bias)` makes them, densest first, their probes numbered one cascade after another (`ProbeCascade::firstProbe`). Each cascade has:

**A window** (`probeWindow`):

- It sits on its spacing's lattice, offset from the world's origin by `probeLatticeOffset` (`DDGI_LATTICE_OFFSET`, half the densest spacing, 0.25 m: the same in metres for every cascade, so they stay nested).
- Its first grid place `start` = the lattice point nearest the centre less (count - 1) / 2 (`probeWindowStart`), so its middle probe is within half a cell of the centre.
- `volume` is a `ProbeVolume` with origin offset + start x spacing and `scroll` start mod count.
- Why the offset: it keeps every probe off whole and half metres, where levels put floors and walls. A probe exactly on a surface starts its rays on it, where they hit or slip through by rounding, and on the light lab's ground (at 0) such probes stored darkness that showed as blotches and bands.
- `ProbeVolume::scroll` (`ProbeScroll`) makes the storage a circular buffer:
  - the probe at grid place l is kept in slot (l + scroll) mod count (`probeSlot`, `probeGridCoord`, `probePosition(volume, slot)`; `probeLatticeCoord(cascade, slot)` = start + grid place);
  - so when the window moves a cell every lattice point keeps its slot and only the plane brought in holds another place (`scrolledProbes(before, after)`; every probe when size or spacing differ).
- Corners, neighbours and event regions are found by grid place, never by slot.

**Its weight** (`probeCascadeWeight(centre, reach, fade, point)`): the product over the axes of

  clamp((reach - |point - centre|) / fade, 0, 1)

- It is centred on the centre itself, not the window, so it moves smoothly and nothing pops when a plane jumps.
- `weightReach` = `probeCascadeReach(count, spacing, biasReach)` = ((count - 1) / 2 rounded down - 0.5 - `DDGI_CASCADE_EDGE` (1)) x spacing - the self-shadow bias's reach, fading over `DDGI_CASCADE_FADE` (1) cell.
- The nearest outer plane is at least (count - 1) / 2 less half a cell from the centre, so the cell round any point lit (found from its biased point) never touches an outer plane, which a scroll may just have brought in (tested over 20,000 random centres and points).
- Reach at the defaults:

  | Cascade | Probes | Full weight round the camera | Up and down |
  |---|---|---|---|
  | densest | 31 x 17 x 31 | 6.1 m | 2.6 m |
  | next | 21 x 13 x 21 | 7.3 m | 3.4 m |
  | fourth | | 29 m | 13 m |
  | fifth and sixth (8 m and 16 m apart) | | 58 m and 116 m | |

- `fixedProbeCascade(volume)` is a volume that stays put, weighted as the single volume was (full out to `DDGI_VOLUME_MARGIN`, half a cell, beyond its outer probes, fading over `DDGI_VOLUME_FADE`), for tests that place probes by hand.

**Packed for the shaders** (`packProbeCascades(cascades, cameraOrigin)`, `GpuProbeCascade`, 112 bytes), one per cascade in a per-frame buffer (`VulkanDdgi::describeProbeCascades`, set 1 binding 20, and bound to the update and sharing passes). Each holds:

- origin from the camera with the longest distance its probes record;
- spacing, scroll with the first probe;
- the weight's centre from the camera, reach and fade;
- and its own counts.

The light lab used to place a 12 x 5 x 16 volume 1 m apart round its building (and a 20 cm test inside it); it now places `ProbeCascades{}`.

### Scene events

Scene events (`ddgi-scene-events`): a game tells the probes about a big change it knows of, which they would otherwise follow only over their memory (2 s), or, with the adaptive memory, once the change stands out from their noise (about a second in the dark room model).

- `ProbeEvent` (`scene/probe_volume.h`):
  - `ProbeEventKind kind`, `LightChange` or `GeometryChange`;
  - `everywhere`, else the probes inside `Aabb region`;
  - `probeEventAround(kind, centre, reach)` makes a cube round a point, such as a light's range, and `probeEventAround(kind, centres, reach)` one box round several, for lights switched together.
- It is a component lasting one frame:
  - `signalProbeEvent(world, event)` (`scene/lighting`) adds it on an entity of its own;
  - `collectLighting` gathers every one into `FrameLighting::probeEvents` in order;
  - and `Application::describeFrame` calls `clearProbeEvents(world)` once the frame is built.
- The renderer passes them to `VulkanDdgi::update` while `GlobalIlluminationSettings::sceneEvents` is on (true), none otherwise.
- `VulkanRenderer::capture` sends them on the first warm-up frame only (`drawFramesBeforeCapture(description, repeated, ...)`, the repeat a copy with no events), not on each of the 960 repeats.
- The light lab signals a light change round its ceiling lamp when L switches it, and everywhere when the clock is moved by hand. The shared flashlight signals one round itself on every switch.

### Relocation and probe states

Relocation and probe states (`ddgi-probe-relocation`; `render/gpu/probe_relocation`, numbers in `ddgi_shared.h`, mirrored by `assets/shaders/probe_relocation.glsl`, tested in `probe_relocation_test`, `probe_relocation_gpu_test` and `ddgi_gpu_test`'s relocation pass): a grid ignores the level, so some probes land in walls (they see back faces and lend darkness) or a hair from a surface, and many have no surface near them at all, which nothing reads.

**Static geometry:** the game marks the level (`ModelRenderer::isStatic`, carried to `DrawItem::isStatic`; the light lab its ground and blocks, the demo its ground and obstacle course).

- A static shadow caster carries `RAY_MASK_STATIC` (4) as well as `RAY_MASK_SOLID` (`rayMaskFor`), so relocation's rays see only the level.
- Unmarked draws count as moving.

**Relocation** (`ddgi_relocate.comp`; `relocateProbe` on the CPU):

- One workgroup of `DDGI_RELOCATION_GROUP_SIZE` (64) per probe asked for: every probe after new cascades, a geometry change or switching relocation, else the planes a scroll brought in. Positions come from the cascades' buffer and the camera.
- The probe traces `DDGI_RELOCATION_RAYS` (256) distance-only rays on the plain Fibonacci spiral (no rotation, so the result repeats) against the static mask.
  - It skips hits nearer than `DDGI_RELOCATION_MIN_DISTANCE` (1 mm), so a probe on a surface or on the seam where two solids touch sees past it to the solid's real inside.
- The first invocation tallies them (`ProbeRayTally`, `tallyProbeRay`):
  - back and front faces;
  - the nearest back face;
  - the nearest and farthest front faces;
  - and how far the nearest front-face hit lies beyond the probe's cells, one spacing each way round its grid point, worst axis first.
- Then it steps (`relocateProbeStep`), up to `DDGI_RELOCATION_ITERATIONS` (5) times, stopping once it stays put or is stuck:
  - **Inside something** (more than `DDGI_INSIDE_BACK_FACE_SHARE`, 25%, of rays meet back faces): out through the nearest back face, the step scaled as far as the offset limit allows less `DDGI_ESCAPE_MARGIN` (1 mm) (`probeEscapeScale`).
    - The offset limit is `DDGI_OFFSET_LIMIT`, 0.4 of the spacing per axis, so the probe stays in its own cell and the grid still finds it.
    - A scale of 1 or less does not reach the face: stuck.
  - **A front face nearer than** `DDGI_MIN_FRONT_FACE_SHARE` (0.1) of the smallest spacing: straight away from it (against the nearest front face's direction) until it stands `DDGI_FRONT_FACE_NUDGE` (0.2) of that spacing off it, clamped to the limit.
    - The 2021 reference moves it towards the farthest front face instead, which inside a room lies out through a door or window: it walked the light lab's one 4 m probe in the back room into the front doorway, and the room seen from afar glowed with the outdoors it saw there (`ddgi-relocation-nudge`).

**The record** (`finishProbeRelocation`, `GpuProbeRelocation`, 32 bytes: offset, off (stuck or still inside), the nearest surface beyond the cells, the largest float when it sees none):

- It goes into the frame in flight's read-back memory in the order asked, and is read when that frame in flight comes round again (its fence has passed, so nothing waits for the GPU).
- `collectAllRelocations` reads every one after the frames in flight change, since a frame in flight no more would never come round.
- It is kept only for a probe still at the lattice point it was asked for at (`ProbeRelocationRequest`, `relocationRequests`, `keepProbeRelocations`).
- A probe a scroll brought in waits pending (`pendingProbeRelocation`: off, never traced or read) for its own record.
- A probe that sees no static surface is not off (the paper's rule): it sleeps, so movers can wake it.

**States** (`ProbeState`, `DDGI_PROBE_*`; `classifyProbe`, `buildProbeStates`, each cascade with its own spacing through `buildCascadeStates`):

| State | Number | Meaning |
|---|---|---|
| off | 0 | buried: never traced or read |
| sleeping | 1 | its nearest static surface lies beyond its cells grown by the bias's reach: no surface reads it |
| awake | 2 | a sleeping probe within a mover's bounds grown by the same and `wakeCells` (1) more cells |
| vigilant | 3 | a static surface within that reach |

- The bias's reach: `probeBiasReach(spacing, bias)` = 0.75 x the smallest spacing x the bias, the longest the self-shadow bias can be.
- Movers are the draws not static with bounds (`movingBounds`).
- Awake and vigilant are traced (`probeTraced`).
- Each frame's `GpuProbeState` (16 bytes: offset, state) per probe.
- With no records yet (or `probeStates` off) every probe is vigilant, and with `probeRelocation` off there are no offsets, which the pass then runs with 0 steps (classify only).

**Restarts:** a probe traced now but not before, or moved (`restartedProbes`), or brought in by a scroll and traced at once (before any records, or with states off), restarts outright through `advanceProbeRestarts(..., restarted, seeded)`:

- no share of either atlas kept on its first frame;
- the event check's threshold 0 (every texel restarts, or with no history starts fresh).

**Seeds** (`seedProbes`, on): of those, all but the coarsest cascade's, at most `DDGI_MAX_SEEDS` (2048) a frame (`chooseProbeSeeds`), once the atlases have been updated more frames than a seed stands for, start from the coarser cascades' light where they stand.

- `ddgi_seed.comp`: one workgroup per seed, one invocation per texel; the trace's sets 0 to 2 plus its own set 3, the seeded probes and the seeds buffer, 8 x 8 texels of two `vec4`s each.
- It reads `probeAmbient` from the next cascade on at the probe (normal, lookup and view all the texel's direction: the irradiance a surface there facing that way gets).
- The update then starts the probe's texels from the seed (`seededProbeTexel`: stored and window mean the seed, history -`DDGI_SEED_FRAMES` (4), noise 0; the fast atlas its seed).
- Its restart counts from there (`GpuProbeRestart::seed`, irradiance frames 4 and the shares kept for 4), so the seed fades as its own rays come in.
- Distances are never seeded.
- **Measured** in the dark room model: over the first quarter second 9.6% error seeded 20% off, 13.3% from its rays alone, 24% seeded with twice or none of the light; from half a second on within half a percent.

**Readout:** `countProbeStates` into `RenderStats::probes`, the F4 line `PROBES n  TRACED n  AWAKE n  ASLEEP n  OFF n  MOVED n` (`probeStatsText`, `probeReadoutLine` 4).

**Measured in the light lab with the cascades** (the defaults, debug build, RTX 5080):

- 3,700 to 5,600 of 22,932 probes traced, about a third off (a window centred at eye height reaches as far below the ground as above);
- the probe pass 0.22 to 0.50 ms;
- from the high overview camera no cascade reaches the ground and every probe sleeps.
- Before the cascades (release, RTX 5080, golden warm-up frames):
  - the 1 m volume moves and switches off no probe, 392 of 960 sleep, the probe pass 0.036 ms instead of 0.050;
  - the dense 20 cm volume: 350 off, 5 moved, about half asleep, 0.25 ms instead of 1.0.

### The API-free part

API-free (`render/gpu/ddgi`, numbers in `ddgi_shared.h`, mirrored by `assets/shaders/ddgi.glsl`, tested in `ddgi_test` and `ddgi_gpu_test`).

#### Octahedral mapping

- Octahedral mapping (`octahedralEncode`, `octahedralDecode`): a unit direction divided by |x| + |y| + |z|, the lower half folded over the diamond's edges, so the sphere covers the square -1..1.
- `probeTexelDirection(texel, side)` is an inner texel centre's direction.
- Each probe's square has a one-texel border (`probeBorderSource`): an edge texel copies the inner texel beside it mirrored about the edge's middle, a corner the opposite inner corner.
  - Checked by carrying each border centre back across its fold onto its source's centre.
  - Numbered for the update's copy by `probeBorderTexel` (4 side + 4 of them, no more than a group's invocations).

#### Rays

- Rays: `sphericalFibonacci(index, count)` (z in equal steps, the golden angle around).
- Frames take turns through one spiral of count × `DDGI_RAY_CYCLE` (16) points:
  - `probeRayFrame(frameNumber)` gives the phase (frame mod 16) and the cycle's rotation (`randomProbeRotation(frameNumber / 16)`, Shoemake's uniform rotation from `mixBits`);
  - `probeRayDirection(ray, count, phase, rows)` is point ray × 16 + phase turned by it (rows from `probeRotationRows`);
  - so a cycle's frames cover the sphere as evenly as 16 times the rays.
- Against independent rotations each frame it cut the light lab back room's crawl from 6.4% to 3.8% at 64 rays (8 and 64 frames a cycle did slightly worse).
- Tested: a cycle traces every point of the long spiral once, sharing its rotation, with every direction within 1.6 share radii of a ray (64 rays and 256), and a sparse texel crawls under 0.7 times as much as with independent rotations.
- Tested: unit, balanced, evenly spread (every direction within 1.6 share radii of a ray), uniform rotations.
- Probes near fast lights trace more rays on the same cycle. Shorter cycles fitted to their 0.05 s memory (a power of two, the phase modulo it, under the same rotation) flickered more at 60, 120 and 240 fps, since a short cycle traces the same directions again and again until the rotation turns.

#### Fast lights

Fast lights (`PointLight::fast`, `SpotLight::fast`; the flashlight is fast): their bounce would trail behind them over the 2 s time constant. Light adds up, so it is kept apart.

- The trace writes each ray's light separately:
  - from still lights (sun, sky, emissive, lamps, the still atlas's bounce);
  - and from fast lights (their direct light at the hit and the fast atlas's bounce, so their later bounces keep the short memory too).
- These are blended into two irradiance atlases with their own time constants (`irradianceTimeConstant` 2 s, `fastIrradianceTimeConstant` 0.1 s), and summed where they are read.
- Probes within reach of a fast light (`probeNearLight(probe, light, range, reach)`: closer than its range plus `probeMaxDistance`, as the hit lights are chosen) trace `fastRaysPerProbe` (256 by default), the rest `raysPerProbe` (256 by default, 1024 on Ultra).
- The fast blend counts a frame as at most `fastBlendMaxFrameSeconds` (1/30 s), so a hitch cannot replace its history with one noisy frame.

**The frame's schedule.** `ProbeSchedule` carries the frame number. `scheduleProbeFrame(schedule, settings, frameSeconds, fastLights, fresh)` makes the frame's `ProbeFrame` (all but the sky):

- ray counts clamped, every probe at the still count when no fast light is listed (`fastLights` false);
- the frame's rotation and phase;
- shares kept from each time constant, 0 for fresh atlases.
- `probeRayStride` (the larger count) is each probe's room in the ray buffers.

**The ray budget** (`probeUpdatesPerSecond`, 0 off): above that many frames a second the still count is divided by `probeRayDivisor(current, fps, budget, rays)`, a power of two, about the largest not above fps / budget.

- A step up is taken `rayBudgetStepUpShare` (5%) short of it, so a jittery 240 settles on 4; a step down only `rayBudgetStepDownShare` (10%) below it.
- Never below `minRaysPerProbe`.
- The frame rate is the frame time smoothed over `rayBudgetSmoothingSeconds` (0.25 s, in `ProbeSchedule`).
- Fast probes keep `fastRaysPerProbe`, the ray cycle stays 16 frames and each frame still blends by its own time, so only the noise changes, and it goes with rays a second.
- **Measured** in `ddgi_test`'s dark-room texel: 16 rays at 240 fps crawl 0.95% against 0.96% for 64 at 60 and 0.33% for 64 at 240. Cycles of 32 and 64 frames for 16 rays measured no better (0.93-0.99%, and worse for a smaller patch), so the shaders are unchanged.
- Tested: the ladder's steps, limits and hysteresis, a steady 240 fps on a budget of 60 settling on 16 rays (fast probes 256), 30 fps tracing all 64.
- Tested without fast lights (64 rays, `probeRayFrame`'s sequence, so the golden images are unchanged) and with them (256 rays, the hitch's capped share).

#### Update maths

- `irradianceRayWeight`: the cosine, none from behind.
- `distanceRayWeight`: the cosine to the power `DDGI_DISTANCE_SHARPNESS`, 50.
- `distanceForUpdate`: a back-face hit, recorded negative, keeps `DDGI_BACK_FACE_DISTANCE_SHARE` (0.2) of its length; everything capped at `probeMaxDistance(spacing)`, 1.5 cell diagonals, which a miss becomes.
- `hysteresisForTimeConstant(frameSeconds, tau)`:

  exp(-frameSeconds / tau)

  After tau seconds 1/e of a step is left, at any frame rate; tested at 30, 60 and 240 fps.
- `blendProbeValue`: irradiance blended linearly, as it is.
  - 2021's fifth roots with per-texel speed-ups settled a sparsely lit texel at half its true light and passed ray noise straight on (`ddgi-sampling`).
  - Tested by a dark room's texel seeing a small bright patch, which must settle within 2% of its mean and move under 4% in 0.1 s.
- The furnace test (a uniform sky stores itself exactly) and a half-lit sky (texels converge on (1 + cos theta) / 2 over 400 rotated frames) are CPU tests.

#### Adaptive memory

Adaptive memory (`adaptiveMemory`, on by default since `gi-defaults`; `adaptiveProbeBlend`, mirrored in `ddgi.glsl`, `AdaptiveProbeTexel`): each still irradiance texel is a running mean that restarts when its light really changes, like a denoiser's history length.

- **The running mean.** The stored light blends each estimate in by

  min(longKeep, n / (n + 1))

  with n its history in frames (the irradiance atlas's alpha, at most `DDGI_MAX_HISTORY` 2048, exact in a half float). So after a restart it is the plain mean of the frames since until the long memory (`irradianceTimeConstant`) keeps more.
- **The change window.** A change window's mean (`changeWindowSeconds`, 0.2 s) and the standard deviation of each frame's luminance about it live in a third RGBA16F image, `changeStatistics`.
  - Luminance is `probeLuminance`, Rec. 709; the deviation is blended by the long memory's share, kept as a deviation so a half float cannot overflow.
  - `VulkanDdgi`: written only by the irradiance update, so it stays in storage use.
- **A real change.** The window's mean leaves the band of `changeThreshold` (8 by default; 5 in the measurements below) times its own spread, plus `DDGI_CHANGE_FLOOR_SHARE` (1%) of the light. The spread is

  sigma sqrt((1 - k) / (1 + k))

  for a mean keeping k a frame. Then the stored light becomes the window's mean with the history k / (1 - k) whose running mean keeps the window's share.
- A texel with no history (fresh atlases, or the memory just switched on: the fixed blend writes alpha 0) starts from its stored light with the history of the long memory's share, so nothing jumps.
- The volume carries the window's share in `rotation[2].w` and the threshold in `sky.w` (0: the fixed blend, exactly as before).
- Fast atlases and distances keep fixed memories.

**Measured** in `ddgi_test`'s dark-room texel, 64 rays, light switched between dark and a 15 degree patch:

- A 4 s long memory against the fixed 2 s comes in within 0.5-1.1 s instead of 4.6 and crawls half as much (60 fps 0.33% against 0.65%, 240 fps 0.11% against 0.23%; 8 s quarters it).
- An 8 degree patch switched off at 60 fps still takes 9 s (its noise fades only with the long memory), a sparse patch's light doubling up to 18 s, and slow drifts lag by the long memory.

Tested: starting from the stored light, the running mean and cap, a restart within 5 frames of a jump from wobbling light, the schedule and packing, and calmer and quicker than the fixed blend at 60 and 240 fps; on the GPU against the mirror (`GPU_TEST_PROBE_ADAPTIVE`).

#### Scene event restarts

Scene event restarts (`advanceProbeRestarts`, `GpuProbeRestart`, `ProbeEventCheck`, `restartedProbeKeep`, `eventCheckBlend`, `restartProbeTexel`, `restartedProbeBlend`, `adaptiveProbeUpdate`, all but the first two mirrored in `ddgi.glsl`).

**What each probe keeps:** the frames since an event reached its irradiance and its distances (negative for none), the share of each it keeps this frame, and the event check's settings.

- `GpuProbeRestart {irradianceFrames, distanceFrames, irradianceKeep, distanceKeep, ProbeEventCheck check {threshold, growth, frames}, fastKeep}`, 32 bytes.
- The shader's `ProbeRestart {ivec2 frames; vec2 keeps; vec4 check}`, `fastKeep` in `check.w`.
- Worked out on the CPU, the update's push constants being full.

**Each frame** `advanceProbeRestarts(restarts, volume, events, frame)`:

1. moves every running restart on a frame, ending it once its ramp, counted from the end of the event check (texels may restart that late), has reached the frame's share kept;
2. then sets 0 for the probes in each event's region (all with `everywhere`): a light change the irradiance, a geometry change the distances too;
   - but only for an atlas the frame keeps some of: a frame keeping nothing, as fresh atlases, has nothing to forget, and a restart started there would run on through the frames after.
3. A volume of a new size starts with none.

**With the adaptive memory** (`adaptiveProbeUpdate(texel, estimate, longKeep, windowKeep, threshold, n, check)`), an event does not restart texels outright but makes each check whether its light really changed, for `eventCheckSeconds` (0.5 s, `check.frames`).

- `eventCheckBlend` keeps the mean of the frames since the event in the change window's place.
- It restarts the texel from it when it and the stored light differ by more than `eventChangeThreshold` (4) times the noise measured before the event, shrinking as 1 / sqrt(frames since + 1), plus `DDGI_CHANGE_FLOOR_SHARE` of the light. Otherwise the stored light keeps its long memory.
- So a big change restarts on the event's frame, a smaller one once enough frames tell it apart, after

  (threshold x noise / change)^2 frames

  and light that hardly changed is left alone, so dim areas an event barely touches do not flicker.
- 0 restarts every texel at once, as events first did.

**A restarted texel** (`restartProbeTexel(mean)`) has its history marked negative while the restart runs (counting its frames since; nothing else reads the irradiance atlas's alpha).

- `restartedProbeBlend` runs while its frames since are fewer than the change window holds (`historyForShare(windowKeep)`): the window's mean is the stored light, the noise measured afresh as the root mean square of each frame's luminance about the mean before it, no change looked for.
- Then `adaptiveProbeBlend`.
- Both use the restart's growing memory:

  `restartedProbeKeep(longKeep, 0, growth, its frames since)` = min(longKeep, exp(-1 / (growth n)))

  so an earlier frame's weight falls as (its time / now)^(1 / growth).
- Growth 1 is close to the plain mean since the restart, whose first frames (the bounce of last frame's probes still in them) fade only as 1 / time.
- When the restart ends the marker goes and the usual memory takes over.

**Why some choices:**

- A new event restarts a still-marked texel at once, unchecked: the light an event changed is most likely switched back by the next, and its noise, measured over the few frames since its restart, is no basis for a check. Checked, a sparsely lit texel toggled faster than its check could decide never restarted, and spamming L left the lamp to fade over the long memory, 13-16 s in the dark room model.
- The check lasts `eventCheckFrames(seconds, frameSeconds)`, at least `minEventCheckFrames` (16), since a sparsely lit texel going dark needs about 13 frames to tell from its noise. A 0.5 s check at 15 fps, 7 frames, never decided: 18 s; now 0.73 s.
- Looking for changes right after a restart, with the old light's noise (a dark room's, tiny) or the first frames' few samples, made the lit room's own noise pass for change after change: 0.27 s to bring a lamp in instead of 0.04.

**The fixed memory** (adaptive memory off) and the distances restart outright and keep `restartedProbeKeep(keep, shortKeep, growth, n)`:

- none on the event's frame;
- never less than a memory of `eventMemorySeconds` keeps a frame (the soft restart, `ProbeFrame::restartHysteresis`), so their first frames are averages.
- With the event check the soft restart no longer applies to the adaptive memory: restarted texels start from a mean of frames, and keeping their first frame longer only kept its noise longer (24.2% against 22.8%).

**Measured** in `ddgi_test`'s dark-room texel (64 rays, a 15 degree patch switched on and off), 30 fps, defaults:

- A texel the lamp makes 15% brighter, or leaves alone, flickers 0.74% (change in 0.1 s) over the 2 s after an event, as with no events, against 13.9% and 14.9% restarted outright.
- The lamp 90% on in 0.16 s and within 1% of darkness 0.28 s after switching off (no events 5.2 and 18.3 s).
- Thresholds of 3 gave 0.09 and 0.13 s, 5 gave 1.3 and 4.9 s.
- Restarting outright (threshold 0), the lamp came in within 0.044 s at 60 fps and 0.008 s at 240 (the adaptive memory alone 0.81 and 0.96 s, the fixed 2 s memory 4.6 s).
- The fixed memory's soft restart, at 30 fps: a texel whose light stays errs 22.8% over the first half second restarted hard, 13.9% with 0.1 s (2.2% left alone).
- Its growth: switched off, within 1% of darkness after 1.32 s at growth 1, 0.30 s at 0.5, 0.20 s at 0.25 (the flashlight's 0.1 s memory 0.46 s), for 3.3%, 4.5% and 7.2% of error from half a second to 4 s in a texel whose light stays.

**The fast atlas** restarts with the still irradiance, outright (it measures no noise to check against, and its memory is short anyway), keeping `fastKeep` = `restartedProbeKeep(fastKeep, 0, growth, n)`, no soft restart (it would keep a switched-off light's bounce).

- The flashlight's switch is an event (`Flashlight::toggle`, a light change over the beam's 50 m reach), so its bounce comes and goes with it.
- In the dark room model with the 0.1 s memory: 90% on within 0.02 s at 30 fps (0.04 at 60) instead of 0.17 (0.22), and dark at once when switched off (the new light exactly none) instead of 0.43 s (0.45) to fall within 1%, with no more flicker after the switch.
- The check lets the region be that generous: still light the flashlight hardly changes is left alone.

**Sharing during a restart.** While a restart runs, the probe also shares its still light more (`eventProbeSharingWeight(weight, eventWeight, n, longKeep)`, mirrored):

- `eventProbeSharing` (0.5) on the event's frame, fading linearly back to the preset's `probeSharing` over `historyForShare(longKeep)` frames, never below it.
- `ddgi_share.comp` reads the restarts (binding 0) and the probes' states (binding 1: untraced probes leave their shared copy alone and are no one's neighbour; the separation and the direction read in the distances are between where relocation moved the two).
- It takes the long memory's share and the event weight in `GpuProbeShare::weight` z and w.
- The fast atlas keeps its own weight.
- It needs the sharing pass running (either sharing weight above 0, as on every preset).

Tested:

- the check (unchanged light never restarts, a far change at once, twice the noise after 5 frames);
- restarted texels' frames, the marker and its end, a new event restarting a marked texel;
- quick switching at 15 and 30 fps (dark at once), the check's least length;
- the ramps, regions, kinds, expiry, fresh frames and resizing;
- the dark-room switch, the faint and unchanged texels' flicker;
- the soft restart and growth on the fixed memory, the fast atlas's restart, the sharing fade, the packing;
- on the GPU against the mirror (`GPU_TEST_PROBE_RESTART`).

#### Sharing between probes

Sharing between probes (`probeSharing`, off by default, the Ultra preset; High 0.1 (0.5 from `gi-defaults` until `gi-preset-sharing`), 0 off, 0 to 1).

- The fast atlas has its own weight, `fastProbeSharing`, 0.5 on every preset since `ddgi-fast-sharing`, against a flashlight's flicker at 30 fps.
- Either above 0 runs the pass and turns the probes' rays.

**How it works:**

- Each probe's irradiance, still and fast, is read as its own light weighted 1 and each of its 6 face neighbours' (`DDGI_SHARE_NEIGHBOURS`) weighted `probeSharing` (or `fastProbeSharing`), over the weights (`sharedProbeValue`).
- A neighbour counts only if the probe's mean distance towards it (its distance texel in that direction, `probeDirectionTexel`) reaches `DDGI_SHARE_VISIBLE_SHARE` (0.9) of the way (`probesSeeEachOther`).
- `ddgi_share.comp` (one workgroup per probe, push constants `GpuProbeShare`, run after both updates; a probe a scene event restarted shares more for a while, see Scene event restarts above) writes the shared copies `sharedIrradiance` and `sharedFastIrradiance` (borders included).
  - The scene set, the debug balls and so the next frame's bounce read them while sharing is on (`VulkanDdgi::prepare` picks).
  - The probes keep accumulating their own light, so the blur never feeds back into their history.
- Each probe also turns its rays its own way before the frame's rotation (`probeRayTurn(probe)`: `randomProbeRotation` of 2^62 + probe, a number no ray cycle reaches; `probeRayDirection(ray, count, phase, rotation, turn)`).
  - The turns come from a buffer of `GpuProbeTurn` rows read by the trace and the update (`packProbeTurns`: identity rows when sharing is off, which leave every ray bit for bit as before; rewritten after `waitIdle` when sharing is switched, the turns being in upload memory the frames in flight read).
  - Why: with one shared turn every probe errs alike and sharing removes nothing. `ddgi_test`: seven probes in the dark-room model at 30 fps, weight 0.5, crawl 1.41% with one turn, 0.54% with their own, against 0.40 of it for independent errors.

**Measured** in the light lab's back room at 30 fps, 64 rays, 2 s (half-float captures 0.1 s apart):

| Sharing | Crawl, RMS | 90th percentile |
|---|---|---|
| none | 1.61% | 1.87% |
| 0.5 | 0.77% | 1.14% |
| 1 | 0.73% | 1.04% |
| none, at 120 fps | 0.88% | 1.36% |

- The room's mean light is 7% brighter at 0.5 (light borrowed from brighter visible neighbours, towards the doorway and the front door).

Tested: turns (repeatable, different, orthonormal, identity exact), visibility, the shared value, direction to texel, the seven-probe model; on the GPU the functions against their mirrors (`GPU_TEST_PROBE_SHARING`), and the real passes: the furnace stays the sky shared or not, and two probes walled apart keep exactly their own light.

#### Grid and atlases

- Grid and atlases: slots numbered x first, then y, then z (`probeIndex`; scrolled, `probeSlot`).
- One set of atlases holds every cascade's probes one after another, whatever their sizes (the densest is larger), numbered from each cascade's first probe.
- Probe i's bordered square sits at column i mod columns, row i / columns (`probeTileOrigin`, `probeAtlasSize`).
  - The columns are the smallest whose square holds every probe, with as many rows as they need (`probeAtlasGrid(probes)`: as near square as whole rows allow; 65,535 probes are 256 x 256 squares, 4,608 distance texels a side).
  - The rows used to be the grid's layers, which needs every cascade the same width and makes a 31-wide one's rows too long (17,298 distance texels).
- A probe's cascade is the last whose first probe is at or below its number (`probeCascadeHolding`; `probe_cascade_holding.glsl`, included after a pass declares `probeCascades[]`).
- `probeAtlasUv(grid, probe, direction, side)` is where to sample.
- `validateProbeVolume` throws for no probes on an axis, spacing not positive, or more than `maxProbesPerVolume` (65,535: one workgroup per probe, every cascade together); any number that fits, the atlases hold.

#### GlobalIlluminationSettings

`GlobalIlluminationSettings` (`RenderSettings::globalIllumination`, F5 Global illumination):

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | true | |
| `probeCascades` | 6 | 4 before `ddgi-far-cascades` |
| `probeSpacing` | 0.5 m | the densest's |
| `densestProbesAcross` | 31 | |
| `densestProbesHigh` | 17 | |
| `cascadeProbesAcross` | 21 | |
| `cascadeProbesHigh` | 13 | the densest's and the coarser ones'; see Placing probes above |
| `seedProbes` | true | see Seeds |
| `shownProbeCascade` | 0 | the debug balls' cascade |
| `raysPerProbe` | 256 | 16 to 1024, the Ultra and High presets; 64 before `gi-defaults`, 128 before `probe-ray-presets`, 1024 from `ultra-dlss-defaults` to `ultra-rt-defaults` |
| `probeUpdatesPerSecond` | 60 | the ray budget, below; 0 off |
| `irradianceTimeConstant` | 2 s | chosen by eye in the light lab's dark back room, against flicker; the price is a few seconds' lag when light changes |
| `distanceTimeConstant` | 1 s | kept steadier |
| `adaptiveMemory` | true | |
| `changeWindowSeconds` | 0.2 s | |
| `changeThreshold` | 8 standard deviations | see Adaptive memory above |
| `sceneEvents` | true | start the frame's `ProbeEvent`s; see Scene event restarts above |
| `eventProbeSharing` | 0.5 | a restarted probe's still sharing weight at first, fading back to `probeSharing`; the presets leave it alone |
| `eventMemorySeconds` | 0.05 s | the soft restart's short memory for restarts without the check, 0 hard |
| `eventMemoryGrowth` | 0.5 | a restart's time constant as a share of the time since, 0.05 to 1 |
| `eventChangeThreshold` | 4 noise units | 0 restarts every texel |
| `eventCheckSeconds` | 0.5 s | |
| `probeSharing` | 0 | the Ultra preset; Low 0.5, Medium 0.3, High 0.1; see Sharing below |
| `fastProbeSharing` | 0.5 | on every preset: the fast atlas's own weight |
| `fastIrradianceTimeConstant` | 0.1 s | since `gi-defaults`, the user's refinement; first 0.05 s: kept up with the flashlight by eye |
| `fastRaysPerProbe` | 256 | the Ultra and High presets (128 before `probe-ray-presets`, 1024 from `ultra-dlss-defaults` to `ultra-rt-defaults`); first 256, when 64 flickered over three times as much |
| `fastLights` | true | keep fast lights apart |
| `fastBlendMaxFrameSeconds` | 1/30 s | |
| `selfShadowBias` | 0.3 | |
| `bounceEnergy` | 0.85 | the share of last frame's probe light a hit passes on |
| `lightsPerHit` | 4 | 0 to `DDGI_MAX_HIT_LIGHTS` 8 |
| `probeRelocation` | true | probes move off the static level; off, they stay on the grid |
| `probeStates` | true | only probes a surface reads are traced; off, every probe |
| `wakeCells` | 1 | 0 to 4: how many cells beyond its own a mover wakes a sleeping probe |
| `showProbes` | false | |
| `probeBallRadius` | 0.1 m | |

Notes on the table:

- The cascade presets: High and Ultra these; Medium five, a densest of 25 x 15 x 25 and four of 17 x 11 x 17; Low four, a densest of 19 x 11 x 19 and three of 15 x 9 x 15 from 1 m. `probeCascadeLayout` lets the densest's up axis give way until it fits one dispatch, then keeps as many cascades as fit beside it.
- `fastIrradianceTimeConstant` measured in the back room: 0.55% of change in 0.2 s at the 90th percentile at 240 fps and 1.6% at 60, against 0.25% for the still light alone, where 0.2 s adds none.
- `fastLights` off: `probeHitLights(..., keepFastApart = false)` lists every light as still, so their bounce goes into the still atlas under its memory, adaptive or not, no probe traces the fast count and the fast atlas fades out.

#### GpuProbeVolume

`GpuProbeVolume` (exactly 128 bytes, every GPU's push constant minimum; `assets/shaders/ddgi_volume.glsl` declares it as `volume`) holds:

- the atlases' columns and rows with the ray stride;
- how many cascades and every cascade's probes together (`volumeProbeCount`, `volumeAtlasGrid`; each cascade's own counts are in its record);
- the rotation's rows, the first two's w the still and the fast ray counts;
- the sky;
- the irradiance and distance hysteresis, the debug balls' radius and the ray phase (whole numbers as floats, read with `volumeWholeNumber`; `volumeRayCount(nearFast)` and `volumeRayDirection(ray, count)` turn them into a probe's rays);
- the fast irradiance's hysteresis.

Where each cascade is comes from the cascades' buffer. `packProbeVolume(counts, cascades, ProbeFrame)`.

### Sampling

Sampling (`render/gpu/ddgi_sampling`, numbers in `ddgi_shared.h`, mirrored by `assets/shaders/ddgi_sampling.glsl`, tested in `ddgi_sampling_test` and `ddgi_sampling_gpu_test`), the 2021 reference code's steps:

**1. The cell.**

- `probeSelfShadowBias(normal, toViewer, spacing, bias)` = (0.2 n + 0.8 v) × 0.75 × the smallest spacing × the setting: the point visibility is tested from, off the surface.
- `probeCell(volume, point)` finds the cell round the biased point (first corner clamped into the grid, `alpha` 0..1 per axis).
- `probeCellCorner` gives its 8 corners (bit 0 x, 1 y, 2 z, clamped past the last probe).
- `probeTrilinearWeight`: each factor at least 0.001; the weights share out 1 and reproduce linear functions.

**2. Per corner:**

  `probeCornerWeight(normal, toProbe, distance, moments, trilinear)` = `crushProbeWeight(probeBackFaceWeight × probeVisibility)` × trilinear

- Back-face weight ((cos + 1) / 2)² + 0.2 from the unbiased offset.
- Visibility 1 up to the stored mean distance; beyond it Chebyshev's bound, cubed, never below 0.05, read from the probe towards the biased point:

  σ² / (σ² + (r − μ)²), with σ² = |m₂ − μ²|

- Weights below 0.2 become w³ / 0.04 (at least 1e-6 first).

**3. The blend.**

- `addProbeSample`/`finishProbeBlend`: the square roots of the stored irradiances, weighted, averaged and squared.
- `ProbeLight {diffuse, specular}`: the irradiance read along the shaded normal, and the same corners and weights read in the mirror direction, the specular part's stand-in until reflections exist (a fully rough reflection).
- `ProbeLights {still, fast}`: both irradiance atlases, each blended on its own with the same corner weights (the screen adds them; a probe ray's hit passes each on into its own atlas).
- `sampleProbes(volume, bias, point, normal, lookupNormal, reflected, toViewer, ProbeLookup)` is the whole loop on the CPU, for tests (furnace, a wall that keeps the dark side's probes out, the mirror direction, each atlas blended exactly as on its own). `ProbeLookup::fastIrradiance` may be empty (no fast light).

**Only traced probes count** (`ProbeLookup::state`; on the GPU the states at set 1 binding 16):

- an off or sleeping corner is skipped;
- each corner sits where relocation moved it (the cell is still found from the grid);
- and a point with no traced corner gets the flat ambient (`ProbeLookup::ambient`), nothing from fast lights.

**The cascades blend densest first** (`sampleProbeCascades(cascades, fromCascade, ...)`, `sampleTracedProbes` on each, which gives nothing when no corner is traced):

- Each adds its light times its weight (`probeCascadeWeight`) times what the denser ones left, until that reaches 1.
- A cascade with no traced probe round the point leaves its share to the coarser.
- The flat ambient takes the rest for still light, and fast light fades out with the cascades, so the edge shows no seam.
- One fixed volume blends exactly as the single volume did: mix(ambient, probes, weight).

**Lights at probe rays' hits:**

- `pointLightAtHit`, `spotLightAtHit`: the irradiance on a matte surface before its shadow ray: radiance × `distanceFalloff` (× `spotCone`) × the cosine, 0 out of range or from behind.
- `hitLightStrength`: Rec. 709 luminance, bloom's weights.
- `keepBrightestLight(BrightestLights, capacity, light, strength)`: brightest first; a light giving nothing never joins; ties keep the earlier.
- `probeHitLights(GpuLights, cameraOrigin, volume)` lists, points first, the frame's lights whose range sphere (`sphereTouchesBox`) reaches the probes' box grown by `probeMaxDistance`.
  - Each is a `GpuProbeHitLight {light, fast}` (from `GpuLights::fastPoints`/`fastSpots`, which `packLighting` fills by slot beside the records, so the main pass's light records keep their layout).
  - It counts the fast ones (`ProbeHitLights::fast`).

**Packing:** `packProbeSampling(GpuFrameData&, ProbeSampling)` fills the frame block's:

- `probeShading` (the bias, the bounce energy);
- `probeAtlas` (the atlases' columns and rows for every cascade's probes together, w how many cascades);
- `probeLights` (listed points, spots, lights kept per hit, capped at 8).
- `GpuFrameData` is 320 bytes.

### Shaders

`scene_probes.glsl` (after `frame_data.glsl`):

- declares the atlases at set 1 bindings 12 and 13 (still irradiance, distances) and 15 (fast irradiance);
- through `scene_probe_states.glsl` the probes' states at binding 16, and through `scene_probe_cascades.glsl` the cascades at binding 20 (both also read by the debug balls; one-record `noProbeStates` and `noProbeCascades` buffers stand in without probes);
- holds the corner loop that reads one cascade (`sampleProbes(cascade, ...)`, returning `ProbeLights` and whether any corner was traced; the specular reads only when asked, a constant at each call);
- and `probeAmbient(point, ..., fromCascade, withSpecular)` (the cascades blended densest first, the flat ambient taking the rest of the still light).
- Everything is measured from the camera, the probe rays' hits too.

`triangle.frag` samples when its specialization constant `DDGI_SAMPLE_PROBES_CONSTANT` is on:

- visibility and back-face weights along the geometric normal, the light along the shaded normal and its mirror direction;
- it shades the light from all around, still plus fast, with `shadeProbeAmbient(point, diffuse, specular)` (`render/gpu/brdf`) instead of `shadeAmbient`.
- The renderer keeps two more main-pass pipelines with it on (`mainProbesPipeline`, `mainAfterPrepassProbesPipeline`; `PipelineDescription::fragmentConstants`) and uses them on frames that trace probes (`tracesProbes`).
- Why a constant: with the code behind a plain branch the driver compiled the rest of the shader slightly differently and moved some 25 pixels a view by a half-float step. The constant keeps it out of the other pipelines entirely.

### VulkanDdgi

`VulkanDdgi` (`render/vulkan/vulkan_ddgi`) is made by `createRayTracing` when the GPU has ray queries and both atlas formats support storage and sampling.

**The atlases**, every cascade's probes in each, all storage, sampled and transfer images, shared by the frames in flight:

- the two irradiance atlases, still and fast (`R16G16B16A16_SFLOAT`, 10 x 10 texels a probe);
- the distance atlas (`R16G16_SFLOAT`, 18 x 18 a probe; the device turns on `shaderStorageImageExtendedFormats` for two-channel storage images).

**Three ray buffers** in GPU memory, each with room for the most rays the settings ask a probe to trace (grown, after waiting for the GPU, when they ask for more):

- the rays (a `vec4` per ray: the light brought back from still lights and the distance);
- the fast rays (the light from fast lights as three half floats in a `uvec2`, `packFastRayLight`, half the size of floats; the still rays stay full floats, so frames without fast lights are bit for bit the same);
- and a flag per probe (a fast light within reach).

Each frame that traces probes runs the steps below.

#### prepare

`prepare(commands, cascades, settings, draws, cameraOrigin, frameIndex)` runs before the frame's scene set is written.

- Set 1 points at the atlases (`sampledIrradiance`, `sampledDistance`, `sampledFastIrradiance` with the linear clamped sampler; without probes a one-texel `noProbesTexture` stands in).
- Cascades of another size or spacing than the last (or the first):
  - wait for the GPU (`waitIdle`, as a shadow atlas resize does);
  - make new atlases cleared to zero (readable, `SurfaceRead`) and new per-probe buffers;
  - and their first update takes the rays outright (hysteresis 0).
- Cascades that only moved forget the planes they brought in (pending, asked to relocate).
- Otherwise the hysteresis comes from the time constants and the frame's length.

Around it, `drawScene`:

- makes the cascades round what `ProbeCascades` follows (`cameraOrigin` by default);
- lists the hit lights (`probeHitLights` against the coarsest cascade's box) before `writeScene` writes them (set 1 binding 14, compute only, an `ivec2` each) and packs the probe fields.

It also:

- reads the relocation this frame in flight asked for when it last ran (`collectRelocation`);
- asks for every probe when the cascades are new or `probeRelocation` was switched;
- works out the frame's states (`buildCascadeStates` from the records and `movingBounds(draws)`, `restartedProbes` against last frame's) into the frame in flight's `stateUploads` (`describeProbeStates` for set 1 binding 16) and the cascades into `cascadeUploads`;
- lists the probes it traces (`chooseTracedProbes`, `render/gpu/probe_relocation`) into the frame in flight's `tracedUploads` (their count, then their numbers), and makes room for their rays (`reserveRays`);
- and chooses the seeds.

#### The traced probes and the ray buffers

- **The list:** every awake and vigilant probe, in order, while there are at most `maxTracedProbes` (`DDGI_MAX_TRACED_PROBES`, 8,192) of them. A typical frame traces 3,000 to 6,000 of the 45,000. Past the cap (before the first relocation records, when every probe counts as vigilant, and with probe states off), a window of 8,192 that moves on by that many each frame (`updatedFrames` the turn), so every one is traced in turn.
- **The ray buffers** hold rays only for the listed probes, each at its place in the list (slot × stride + ray): room for the most rays the settings ask for (`rayCapacity`) times the most probes listed (`tracedCapacity`), grown when either needs more (waiting for the GPU then). The flags of which probes a fast light reached stay per probe.

  | Rays a probe | The ray buffers (24 bytes a ray) | For every probe, as before |
  |---|---|---|
  | 128 (default) | 25 MB | 138 MB |
  | 256 (Ultra) | 50 MB | 276 MB |
  | 1024 (the most) | 201 MB | 1.1 GB |

- **Rays a probe:** `DDGI_MIN_RAYS` 16 to `DDGI_MAX_RAYS` 1024 (it was 256 until `ddgi-more-rays`).

#### update

`update(commands, descriptors, sets, volume, settings, sky, frameSeconds, fastLights, events, frameIndex)` (`fastLights`: a fast light is listed; `events`: the scene events to start):

1. schedules the frame (`scheduleProbeFrame`, its `ProbeSchedule` kept from frame to frame);
2. moves the restarts on (`advanceProbeRestarts` into `restarts`, kept from frame to frame) and writes them into the frame in flight's `UploadArray` (`restartUploads`, one per frame in flight, read where they are written: no copy, no barrier, the frame's fence having ended its last reads);
3. traces and blends;
4. then leaves the atlases `SurfaceRead` again: sampled by the main pass, the debug balls and the next frame's trace.

Before the trace:

- When a relocation is asked for, or an event is a `GeometryChange` (every probe), it records the relocation pass first, for a later frame's states (`relocate`: the scene's sets 0 to 2 plus set 3, the probes asked for and the frame in flight's records in read-back memory; push constants `GpuProbeRelocate`, each cascade's counts and the most steps; a barrier to host reads).
- Then the seeds (`seed`, when any; the seeds buffer waits for last frame's update reads before it is written, and this frame's update for the writes).

#### Trace

Trace (`ddgi_trace.comp`, `DDGI_TRACE_GROUP_SIZE` 64 invocations a group, one per ray of the stride for each listed probe): the scene's sets 0 to 2 (textures, the frame's scene, the ray tracing set) plus set 3: the three ray buffers, the probes' turns and the traced probes (binding 4).

- Each invocation finds whether a fast hit light is within its probe's reach (`anyFastLightNear`, the lights' positions back in world space), and so its probe's ray count (`volumeRayCount`).
- Ray 0 writes the probe's flag, and invocations past the probe's count stop at once (with 64-invocation groups and counts that are multiples of 64, whole groups).
- Each ray goes from its probe against solid instances (`RAY_MASK_SOLID`, no minimum distance, reach `DDGI_RAY_REACH`):
  - **a miss** brings back the sky (`FrameLighting::background`) at the full reach;
  - **a back face** no light, at minus its distance;
  - **a front face** its material's emissive (times its emissive texture at full size) plus its base colour (texture at full size times the factor) times the irradiance there, which is the sum of:
    - each directional light's radiance times the cosine when a shadow ray (`gl_RayFlagsTerminateOnFirstHitEXT`, from `DDGI_SHADOW_RAY_OFFSET` (1 cm) out along the normal) reaches it;
    - the brightest `lightsPerHit` of the listed point and spot lights (ranked unshadowed, still and fast together), each with a shadow ray to its centre (its glowing stand-in is not solid), a fast one's light into the fast sum;
    - and last frame's probes at the hit (`probeAmbient`, diffuse only, seen from the probe, in world space) times `bounceEnergy`, the still atlas's into the still sum and the fast atlas's into the fast sum, so each frame adds a bounce.
- Matte only: probes average gloss away. The colour is the matte one (`matteColour`: none for metals).
- With mirror pairs, `ddgi_mirror.comp` follows over the same rays, adding the light mirrors reflect onto the fast rays' hits (see Mirrored lights, The bounce).
- Shadow rays rather than the shadow atlas, which holds only what the camera sees (the sun's cascades end at the farthest surface on screen).
- Only the listed probes trace (awake and vigilant ones), from where relocation moved them (set 1 binding 16).

#### Update passes

Update (`ddgi_update_irradiance.comp`, `ddgi_update_distance.comp`, both `ddgi_update.glsl` with `PROBE_TEXELS` and `UPDATE_IRRADIANCE`):

- Bindings: the three ray buffers at bindings 0 to 2, the probes' turns at 3, the traced probes at 4, the restarts at 5, the probes' states at 6, the cascades at 7 (for each one's longest distance), the seeds at 8, the atlases from 9.
- One workgroup per listed probe, one invocation per inner texel (8 x 8, 16 x 16).
- The probe's flag gives its ray count (`volumeRayCount`). Their directions and results go into shared memory `DDGI_UPDATE_RAY_CHUNK` (256) at a time, each texel adding a chunk into its sums in ray order between two barriers, so the sums are the same for any count. 1024 rays at once would take about 48 KB, NVIDIA's limit.
- Each invocation sums its weights over all of them, keeps its old value when the summed weight is below 1e-6, else blends (irradiance as it is, the still and fast sums into their own atlases with their own shares kept; distances as mean and mean square), and writes in place with `imageStore`.
- After a barrier the group copies the border from shared memory.
- A probe not in the list has no group: its atlases are kept as they are.

#### Barriers

- The ray buffers wait for last frame's update reads before the trace writes, and the trace's writes before the update reads.
- Each atlas goes to `ComputeStorage` from `SurfaceRead` (waiting for this frame's trace and last frame's main pass and balls to finish reading), and back to `SurfaceRead` after the blend.

### Golden images with probes

Golden images: a view with probes is drawn `goldenProbeWarmUpFrames` (960, 8 s: four irradiance time constants, `engine/golden.h`) times first, each a tick long, so its probes have gathered their light and bounces.

- This is `VulkanRenderer::capture`'s `probeWarmUpFrames`, `drawFramesBeforeCapture`, which also measures the depths for the cascades.
- The same commands from the same start, so the capture repeats exactly.
- `Application::captureGoldenView(view, probeWarmUpFrames)` takes another count for measuring: two captures N and N + k frames apart are frames N and N + k of one run, so their difference is how much the probes' light flickers in k frames.
- The light lab's `flashlight` view (the dark back room lit by the flashlight) covers the fast atlas, and `back-room-lamp` (the back room lit by its ceiling lamp) a still lamp's bounce.

### Debug view

Debug view (`showProbes`, F5): `drawProbeBalls` (`GpuPass::ProbeBalls`, BALLS) after the main pass, which then stores its depth.

- The scene image and depth are loaded.
- `ddgi_probes.vert` draws a square per probe (6 corners, one instance each, no vertex buffer) through the ball's centre square-on to the line from the camera, half side

  r d / sqrt(d^2 - r^2)

  (a square facing the camera's forward axis cut off balls near the view's edges).
- `ddgi_probes.frag` meets the ball along the pixel's ray (discarding misses; the device turns on `shaderDemoteToHelperInvocation`, which Vulkan 1.3 requires and `discard` compiles to), writes its true depth (`GREATER`, written) and shows the stored irradiance (still plus fast) in the direction of its normal, which is how a white matte ball lit by the probe would look.
- Layout: the scene's sets 0 and 1 (the camera from the frame data) and both irradiance atlases as set 2, the probes' numbers (`GpuProbeVolume`) as vertex and fragment push constants.
- In the light lab's sunlit room with only the sun and sky the balls near the window glow with sky light, low balls are lit on the side facing it, and balls facing only unlit walls are black, as one bounce should leave them.
- Each ball sits where relocation moved its probe, one cascade's at a time (`shownProbeCascade`, 0 the densest; the cascades nest, so coarser probes sit where finer ones do), as much larger than the densest's balls as its spacing (the vertex shader passes the centre and radius on).
- Untraced probes show their state instead of their stale light: off red, sleeping dark blue, and an awake probe its light inside a yellow rim.

## Sky and atmosphere

The sky and the air the light crosses (`sky-atmosphere`; Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"; the physics in `docs/ENGINE_GUIDE.md`, Light through the air).

### The Atmosphere component

`Atmosphere` (`scene/atmosphere.h`, a component, collected into `FrameLighting::atmosphere`, the first). `visit` names the scalar numbers.

| Field | Value |
|---|---|
| the planet's and the air's top radii | 6,360 and 6,460 km |
| Rayleigh scattering | (5.802, 13.558, 33.1) per 1,000 km with an 8 km scale height |
| aerosols | Mie scattering 0.003996 and absorption 0.0044 per km, 1.2 km scale height, Henyey-Greenstein g 0.8 |
| ozone absorption | (0.65, 1.881, 0.085) per 1,000 km in a tent 15 km either side of 25 km |
| the ground's albedo | (0.3) |
| `groundAltitude` | the world's y = 0 above the surface |
| `sunDiscScale` | the disc's drawn size, set by the game |

### The API-free part

API-free (`render/gpu/atmosphere`, numbers in `atmosphere_shared.h`, mirrored by `assets/shaders/atmosphere.glsl`, tested in `atmosphere_test` and `atmosphere_gpu_test`).

**Coordinates:** positions in the planet's frame, km, y up, the camera at (0, viewRadius, 0) (`atmosphereViewRadius(atmosphere, cameraY)`: the ground's altitude plus the camera's height, at least 1 m).

**The medium and geometry:**

- `atmosphereMedium(altitude)` (scattering and extinction), `rayleighPhase`, `henyeyGreensteinPhase`.
- `rayDistanceToSphere(radius, cosZenith, sphereRadius)`: from the radius, (r - R)(r + R), the near root from the far one. A float's last digit at the planet's radius is half a metre.
- `aboveHorizon`.

**Marching:**

- `marchSample`: steps growing as the square of their number, sampled at their midpoints.
- `atmosphereMarchLength`: to the ground or the top.
- `atmosphereTransmittance(radius, cosZenith)`: 40 steps; 0 below the horizon, 1 above the air.
- `scatteringMarchStep`: the paper's energy-conserving step, with `oneMinusExp`'s series for thin air:

  (source - source T) / extinction

- `skyStepSource`, `groundRadiance` (albedo x sunlight x cosine / pi).
- `multipleScatteringDirection` and `multipleScatteringFromSums` (`multipleScatteringTexel`: 64 Fibonacci directions of 20 steps, isotropic, the ground at their ends):

  mean light / (1 - mean share scattered once)

**The tables' coordinates** (texel centres on the ends):

- `transmittanceLutUv`/`Ray`: Bruneton and Neyret's;
- `multipleScatteringLutUv`/`Ray`: the sun's cosine across, the altitude down;
- `skyViewLutUv`/`Direction`: the zenith angle down, the sky above and the ground below the horizon each squared towards it, the bearing from the sun across.

**Lookups and results:**

- `skyViewLookup(world direction, towardsSun)`.
- `skyRadiance` (the CPU's sky view texel, `AtmosphereLookups`).
- `skyAmbient`: 4 / N times the sum of sky x up over 256 Fibonacci directions above.
- `throughAtmosphere`: a directional light times the transmittance from the camera towards it.
- `atmosphereSun`: the brightest directional light.
- `packAtmosphere` (`GpuAtmosphere`, 144 bytes): coefficients, radii and the camera's, the ground and a present flag, the sun's direction and the drawn disc's cosine, its illuminance pi x value above the air, the disc's radiance, the frame's own ambient.

### VulkanAtmosphere

`VulkanAtmosphere` (`render/vulkan/vulkan_atmosphere`, made with the renderer) holds the three tables (RGBA16F storage images) and the flat ambient buffer (one vec4, GPU memory).

`update` (GpuPass `Atmosphere`, SKY in F4, after the ray scene and before the probes) runs:

1. `atmosphere_transmittance.comp` and `atmosphere_multiple_scattering.comp` (one workgroup per texel, one invocation per direction) when the air changed (`sameAir`);
2. then `atmosphere_sky_view.comp` (lit through the transmittance and multiple scattering tables, `atmosphere_tables.glsl`, plus the sunlit ground);
3. and `atmosphere_ambient.comp` (one workgroup of 256, summed in order).
4. Barriers order each frame's writes after the last frame's reads.

`drawSky`, inside the main pass after the draws: `fullscreen_triangle.vert` at the far depth with the depth test equal, `sky.frag` adding (additive blend) the sky view table along the camera's ray (`GpuRayDebugView`, as the ray debug view) and the disc's radiance inside its drawn radius.

**Measured:** 0.015 ms a frame on the RTX 5080.

### In the renderer

The renderer (`drawScene`), with an atmosphere:

- Every directional light becomes `throughAtmosphere` of itself before light priority ranks them (on a copy of the frame's lighting), so the brightest light as it reaches the ground gets the shadow cascades: once the sun has set, the moon.
- `packFrameAtmosphere` writes the frame's `GpuAtmosphere` (`SceneBuffers::atmosphere`, set 1 binding 19, zeros and absent otherwise, 192 bytes) with:
  - the sky's lights (`atmosphereSkyLights`: the sun, the brightest directional light that shines by itself, and the moon, the brightest with `DirectionalLight::reflectsSun`);
  - and the sun disc's radiance from `discRadiance` (`scene/sky_object.h`) of the sun through the air at its drawn size.
- Set 1 binding 17 is the flat ambient everything reads (`scene_sky.glsl`, `flatAmbient()`, in place of the frame block's `ambient`): the atmosphere's buffer with one, else `SceneBuffers::skyLight`, the frame's own ambient (so frames without an atmosphere draw exactly as before).
- Binding 18 is the sky view table (the no-probes texture without one), which probe rays that escape add to the background (`skyLightTowards`).

### The moon and the stars

The moon and the stars (`night-sky`).

**Two skies.** The sky view table holds one sky per light that lights it (`ATMOSPHERE_SKY_VIEW_LIGHTS`, 2: the sun's, then the moon's below it, 192 x 216 in all; `skyViewAtlasUv(uv, light)`, each sky keeping its own texel centres so filtering never reads the other).

- Scattering is linear in the light, so the moon's sky is the same march with the moon's direction and illuminance (`atmosphereLightDirection`, `atmosphereLightIlluminance`; `skyStepSource` takes the illuminance), a light giving nothing skipping its march.
- `skyFromTable` adds both skies, each looked up by its own light's bearing, so the sky pass, the flat ambient and probe rays that escape all gain the moonlit sky.

**The moon's disc** (`sky.frag`) is a ball lit by the sun: `lommelSeeliger(direction, towardsMoon, sinRadius, towardsSun)`,

  2 mu0 / (mu0 + mu)

for the normal the direction maps onto straight across the near half (1 over the whole full moon, edge and middle alike), times `GpuAtmosphere::moonDisc`.

- `GpuAtmosphere::moonDisc` is the moon's light through the air over the disc (`discRadiance` at `angularRadius x Atmosphere::moonDiscScale`) divided by `lommelSeeligerDiscShare(phase angle)`, the shading's average over the disc:

  1 - sin(a/2) tan(a/2) ln cot(a/4)

  so the lit part sums to the moon's light.
- None without a sun or below a share of `minMoonDiscShare` (1e-4).

**The stars** (`render/gpu/stars`, mirrored by `assets/shaders/stars.glsl`, tested in `stars_test` and `stars_gpu_test`; `render/vulkan/vulkan_stars`, `stars.vert`/`stars.frag`):

- `FrameLighting::stars` (a `StarrySky`, `scene/stars.h`, the first) draws after the sky in the main pass with an atmosphere, one instance of six vertices per star (no vertex buffer), additive, at the far depth with the depth test equal.
- Each star is turned into the world (`StarrySky::toWorld`), dimmed by the transmittance table (none below the horizon), and hidden behind the camera or the moon's disc.
- Its light is

  `starLight(magnitude, brightness, contrast)` = brightness 10^(-0.4 contrast m)

  times pi and its colour, divided by its pixel's solid angle (`pixelSolidAngle`: (2|right|/width)(2|up|/height) cos^3 off the axis).
- It is shared over a square a pixel either side of where it falls (`starScreenPosition`) by the tent `starSplatWeight` (1 - |x|)(1 - |y|), which sums to 1 wherever the star falls, so it never flickers.
- Its records (`GpuStar`, 32 bytes: equatorial direction, magnitude, colour) sit in an upload buffer per frame in flight, rewritten only when the frame's catalogue changes.
- Its push constants (`GpuStarView`, 128 bytes) are the sky pass's camera rays and the rotation's rows, brightness and contrast in w.

### Tests

Tested:

- the closed-form transmittance straight up, its fall towards the horizon and reddening;
- the medium, phase normalisation, sphere distances, the tables' coordinates both ways, the lookup, the step;
- multiple scattering by the sun's height;
- a blue noon, symmetry and proportion, a red sunset, the ground, the ambient's units, the view radius, sunlight through the air, packing and collection;
- on the GPU every library function against its mirror, and the real passes against the CPU (tables to half-float rounding, the sky view and ambient within 1% through the tables as read back).

## Temporal anti-aliasing

Temporal anti-aliasing (`taa`; `render/gpu/taa`, numbers in `taa_shared.h`, mirrored by `assets/shaders/taa.glsl`, tested in `taa_test` and `taa_gpu_test`; `render/vulkan/vulkan_taa`): each pixel averages what it saw over about the last ten frames, while the projection moves by a different fraction of a pixel every frame, so edges, thin things and sharp highlights get many samples for the cost of one. It also upscales (`render-scale`): the scene may be drawn smaller than the output, and the history, at the output's size, is built up from the jittered samples (TAAU; see Upscaling below).

### Jitter

- `taaJitter(frame)`: the Halton (2, 3) point of the frame's place in a cycle of `TAA_JITTER_FRAMES` (16), counted from 1, minus a half: within half a pixel of the centre. `halton(index, base)` is the radical inverse.
- `jitteredClip(viewProjection, offset, width, height)` adds offset × w to clip x and y (2 / width of NDC a pixel, y negated since pixels count down), so every point on screen moves the same and depth is unchanged.
- `drawScene` jitters `relativeViewProjection` (the main pass, the depth prepass, the probes' debug balls) by `taaJitter(taaFrame)` render pixels (on the render size, `sceneSize`) when TAA and `TaaSettings::jitter` are on, and passes the same jitter to the resolve. `viewProjection` (culling, debug lines), the shadow views, the sky's rays and the stars stay unjittered. `motionViewProjection` keeps this frame's camera without the jitter.

### Motion

**Still things and the sky** are reprojected from depth:

- `previousRelativeViewProjection(previous, previousOrigin, currentOrigin)`: last frame's unjittered camera-relative view-projection times the translation by the camera's step, so it takes points measured from this frame's camera (the numbers stay small).
- `taaReprojection(current, previous)`: previous × `inverse(current)` (`math/mat4`'s general inverse, Gauss-Jordan in doubles with partial pivoting), exactly the identity when the two are equal, so a still picture's history is read where it was written.
- `reprojectPixel(reprojection, pixel, depth, size)`: the pixel's NDC (`pixelToNdc`; (-1, 1) the top-left corner, as the flipped viewport draws) and depth through the reprojection; valid only when w stays above 0 (in front of last frame's camera). The far plane (depth 0) reprojects by the camera's turn.

**Moving objects** get motion vectors:

- `DrawItem::previousModel`: the matrix the same part was drawn with last frame. `MotionHistory` (`scene/frame_description`) keys parts by entity (slot and generation) and part index; `buildFrame(world, camera, alpha, &history)` fills and remembers them; `Application` owns one (render-only, outside the simulation). None on an entity's first frame.
- `drawScene` lists the visible draws whose `previousModel` differs from `model` and batches them by mesh after the shadow tiles' entries in the draw list. Set 1 binding 21 (`PreviousModelsBinding`) holds every instance's previous model matrix (its current one when unknown).
- The velocity pass (`drawMotion`, GpuPass `Motion`, MOTION): the velocity image (`R16G16_SFLOAT`) cleared to `TAA_NO_MOTION` (1000), then `motion.vert`/`motion.frag` draw the moving objects over the main pass's depth (tested `GREATER_OR_EQUAL`, not written; back faces culled but a double-sided batch's; positions and texture coordinates, a cut-out's holes thrown away in `motionCutOutPipeline`), placing each vertex as the main pass does (jittered) and writing `objectMotion`: this frame's unjittered NDC minus last frame's, from `GpuMotionView` (two matrices, 128 bytes of vertex push constants; the scene layout's push range is the larger of it and `GpuShadowView`). No motion when either clip w is at or below 0.
- `previousPixelFromMotion(pixel, motion, size)` turns a motion back into last frame's pixel.

### The resolve

`taa.comp` (GpuPass `Taa`, TAA, after the depth measurement and before eye adaptation), one invocation per output pixel in 8 × 8 groups (`TAA_GROUP_SIDE`), push constants `GpuTaaPass` (112 bytes: the reprojection, the output size and its inverses, the render size and this frame's jitter, the current weight, the clip width, whether the history is valid, the motion view):

1. **This frame's colour, rebuilt** from the 3x3 render pixels round the one whose sample is nearest (render pixel t shows the unjittered picture at t + 0.5 - jitter, so the nearest to the pixel's place r on the render screen, `renderPixelOf`, is floor(r + jitter)). Each weighs `taaSampleWeight(d²)` = exp(-`TAA_SAMPLE_FALLOFF` d²) (2.29: Karis's Gaussian fit of Blackman-Harris), d its sample's distance in output pixels (`sampleOutputPixel`); the colour is their weighted mean.
2. The same 3x3 pixels in YCoCg (`toYCoCg`, `fromYCoCg`: Y = R/4 + G/2 + B/4): their mean and standard deviation.
3. Last frame's pixel, at the nearest of the 3x3 to the camera (depth dilation, the largest reversed-Z depth, so a moving object's edge moves with it): from the velocity image there where it holds a motion (below `TAA_NO_MOTION_TEST`, 500), else by reprojecting the output pixel with that depth. Off screen, behind the camera or without a valid history, the pixel starts afresh from this frame.
4. The history read with Catmull-Rom (`catmullRomWeights`: four weights per axis; nine bilinear reads, the middle two texels of each axis read as one), clamped to positive.
5. Clipped into the box mean ± `clipSigma` deviations along the line to its centre (`clipToBox`, Playdead's clip; a flat box takes it to the centre).
6. Blended: `taaBlend(current, history, share)`, each side times its Karis weight (`karisWeight`, bloom's 1 / (1 + luminance)), so a lone bright sample does not flash. The share is `taaCurrentShare(currentWeight, nearest sample's weight)`: the setting's times the nearest sample's weight, so a pixel no sample landed close to this frame keeps its history. At the output's size the nearest sample lies within half a pixel's diagonal (weight 0.32 at worst, about 0.8 on average), so the average runs a few frames longer than the setting alone.
   Where the reactive mask (Glass and blended surfaces) says glass changed the pixel, `taaReactiveShare(share, reactive)` = share + (1 - share) × reactive takes that part from this frame alone.
7. `showMotion` writes the motion in pixels × `TAA_MOTION_VIEW_SCALE` (0.1) as red and green instead (blue: no history).

`VulkanTaa` owns the velocity image (at the render size) and two `R16G16B16A16_SFLOAT` histories at the output size that take turns. The resolve writes this frame's history as a storage image and leaves it readable (`getResult()`): it is the finished picture, `VulkanRenderer::finishedScene`, which eye adaptation, bloom, tone mapping and captures read (the scene image itself while TAA is off). The main pass stores its depth whenever TAA is on.

**Barriers:** the velocity image colour target to compute read; the reactive mask already compute read; the depth to compute read; last frame's history from `SurfaceRead` (or a capture's copy source) to compute read; this frame's (discarded) to storage, then to `SurfaceRead`.

**History resets:** the first frame, a change of the output's size (`ensureSize`; a new render size remakes only the velocity image and keeps the history), TAA switched off (`forgetHistory`, and last frame's camera forgotten), and the start of every capture, which also restarts the jitter cycle.

### Upscaling

`UpscalingSettings` (`render/gpu/upscaling`, `RenderSettings::upscaling`, the Upscaling group; tested in `upscaling_test`):

- `renderScale` (1 by default, `minRenderScale` 0.33 to 1): the render size's share of the output's, each way. It applies while anti-aliasing is on (`appliedRenderScale`: TAA, FSR or DLSS), since they are what upscale; with it off the frame is drawn at the output's size.
- **Modes** (`UpscalingMode`, `upscalingModeNames`): Native, Quality, Balanced, Performance and Ultra performance, FSR's and DLSS's names for render scales 1, 1/1.5, 1/1.7, 1/2 and 1/3 (`upscalingModeScale`). Not stored: the debug UI's combo sets the scale, and `matchingUpscalingMode` reads a scale back as its mode within 0.005 (half a slider step), else Custom. The quality presets leave the scale alone.
- `renderSize(width, height, scale)`: each side rounded, at least one pixel and at most the output's. `ensureFrameTextures(width, height, settings)` keeps the scene image, its depth, the velocity image and the clusters' lists at it (`sceneSize`) and remakes them when it changes, like a resize.
- At the render size: the depth prepass, main pass, sky, stars, probe balls, ray debug view, velocity pass, clusters and the depth measurement. At the output's: the histories, eye adaptation, bloom, tone mapping, the debug lines and UI. The shadow atlas, the probes and the sky's tables do not depend on the screen.
- `textureLodBias(render, outputWidth)`, log2 of the render width over the output's (-1 at half), goes to `GpuFrameData::textureOptions.x`: the mips are chosen for the output's pixels, and the history averages the sharper level's detail.
- The jitter cycle stays 16 frames at every scale for the engine's own resolve. Fewer samples reach each output pixel per cycle as the scale falls (NVIDIA suggests 8 / scale² phases for DLSS); a longer cycle is not tried yet for it. FSR gets its own (`jitterPhaseCount`, below).

### FSR

`UpscalingSettings::upscaler`, the one anti-aliasing choice ("Anti-aliasing" in the Upscaling group: `Upscaler::Dlss` by default (TAA where DLSS is missing), or `Off`, `Taa`, `Fsr`, "FSR 3.1" in the settings file; `temporalAntiAliasing` is any but Off, which draws with no jitter, motion or history; it replaced `TaaSettings::enabled` in `taa-choice`) and `sharpness` (0.8, its RCAS sharpening after it upscales; 0 turns it off). FSR and DLSS replace the resolve but use its jitter and velocity pass; FSR runs only where `VulkanFsr::isAvailable()`: `activeUpscaler` falls back to TAA otherwise, and the F4 readout's exposure line ends with the upscaler in use and the render scale (`upscalerText`, e.g. `FSR 3.1 AT 0.67`).

**The DLL** (`render/vulkan/vulkan_fsr`, `VulkanFsr`): `SDL_LoadObject("amd_fidelityfx_vk.dll")` and the FidelityFX API's C functions by name (`ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`). A C interface is what lets MSVC's DLL be called from GCC. The makefile copies the DLL beside each build's exe (`$(FSR_DLL)`), and `tools/package_demo.py` packages it. It imports Microsoft's C++ runtime (`MSVCP140`, `VCRUNTIME140`): without it, or without the device features, FSR is unavailable and the game carries on. SDK v1.1.4 is the last line with a Vulkan backend (2.x, with FSR 4, is DirectX 12 only).

**The context** is made at the first FSR frame for renders up to the output's size (`maxRenderSize` = `maxUpscaleSize` = the output, so a new render scale needs no new context), with `HIGH_DYNAMIC_RANGE`, `DEPTH_INVERTED` and `AUTO_EXPOSURE` (as AMD recommends; the engine's own exposure is not passed), and `DEBUG_CHECKING` in debug builds, its messages in the log. `ensureSize` (from `ensureFrameTextures`, GPU idle) remakes the output image (`R16G16B16A16_SFLOAT`, storage, sampled, transfer source) and drops the context when the output's size changes.

**Each frame** (`drawScene`, timed as the TAA pass):

1. The jitter cycle is `jitterPhaseCount(render, outputWidth)` = ceil(8 n²) phases, n the output over the render width (8 at full size, 18 at 1.5, 32 at 2, 72 at 3), as AMD's table.
2. The camera motion pass (`motion_fill.comp`, `VulkanTaa::fillCameraMotion`, `GpuMotionFillPass`): every velocity pixel still at `TAA_NO_MOTION` gets `cameraMotion(reprojection, pixel, depth, size)` (this frame's NDC minus last frame's; none behind last frame's camera), so the velocity image (now with `STORAGE`) holds a motion everywhere.
3. `VulkanFsr::dispatch` with `makeFsrFrameInputs`: the jitter in render pixels (FSR's convention is `jitteredClip`'s), `motionVectorScale` = (-w/2, +h/2) of the render size (turning NDC now minus then into render pixels from now to then, y down), the frame time in milliseconds, the sharpening, near and far swapped (FSR names them by depth for reversed-Z), the vertical field of view, and `reset` when FSR did not draw the last frame (`fsrHistory`) or the camera is unknown. The colour, depth and velocity are moved to `ComputeRead` (`SHADER_READ_ONLY_OPTIMAL`, depth too) and the output to `ComputeStorage` (`GENERAL`), the states it is told and returns them to; then the output to `SurfaceRead`, and it is `finishedScene`.
4. The engine's history is forgotten while FSR draws (and FSR's dropped while TAA does), so each starts afresh when switched back. If the context cannot be made, FSR becomes unavailable and the frame falls back to the resolve.

The textures' mip bias with FSR (and DLSS) is `log2(scale) - 1` (`textureLodBias(..., Upscaler::Fsr)`), AMD's and NVIDIA's recommendation. Both get the reactive mask (Glass and blended surfaces, below): FSR as its `reactive` input, DLSS as `kBufferTypeBiasCurrentColorHint`. The sky and stars need none (they have depth and camera motion). Golden captures use whatever the settings say (TAA by default).

### DLSS

`Upscaler::Dlss` ("DLSS"), NVIDIA's DLSS super resolution through Streamline (`render/vulkan/vulkan_dlss`), on RTX GPUs; `activeUpscaler` falls back to TAA where `VulkanDlss::isAvailable()` is false.

**Why Streamline:** DLSS's own SDK (NGX) is reached through static libraries built with Microsoft's compiler, which GCC cannot link (they call into Microsoft's C++ runtime). Streamline is a DLL with `extern "C"` functions and plain structs (single inheritance from a 32-byte base with no tail padding, fixed-size enums), so it is loaded like FSR. A trial before the integration (`slInit`, the requirements, a device, `slSetVulkanInfo`, `slDLSSGetOptimalSettings` passing structs both ways, and `slInit` again after `slShutdown`) worked from GCC.

**The files:** Streamline's headers are MIT and committed (`sl_nvperf.h`, under other terms, left out). Its DLLs (`sl.interposer`, `sl.common`, `sl.dlss`, `NvLowLatencyVk`, and `nvngx_dlss`, DLSS itself, under the NVIDIA RTX SDK licence, which lets an application ship it but not hand it out alone) are fetched by `tools/fetch_streamline.py` (the pinned release, SHA-256 checked) into git-ignored `external/streamline/bin`; the makefile copies them beside each build's exe (`$(STREAMLINE_DLLS)`), and `tools/package_demo.py` packages them with their licences and a DLSS credit in the zip's README, as the licence asks.

**`StreamlineLibrary`** (a member of `VulkanRenderer` before its `VulkanDevice`, since Streamline must start before the device exists):

- `sl.interposer.dll` beside the exe, its signature checked with Streamline's own `sl::security::verifyEmbeddedSignature` (WinTrust: `-lwintrust -lcrypt32`), loaded once a run and never unloaded (the golden images make one renderer per game);
- `slInit` with `eUseManualHooking` (the engine makes and calls Vulkan itself), `eUseFrameBasedResourceTagging`, `eDisableCLStateTracking`, Vulkan, DLSS, plugins from the exe's folder, no over-the-air updates, its information messages dropped and two kinds of warning it repeats or expects (the validation layers' own, and the state-tracking hooks manual hooking leaves out);
- `slGetFeatureRequirements(DLSS)` gives the extensions (`getExtensions`), passed to `VulkanDevice` (all but `VK_EXT_buffer_device_address`, which Vulkan forbids beside the KHR one);
- `shutdown()` from the renderer's destructor, after the GPU is idle and before the device goes.

**`VulkanDlss`** (after the device):

- `slSetVulkanInfo` (the device, instance, GPU, and its queues after the engine's one; DLSS asks for none) and `slIsFeatureSupported`; the swapchain presents through Streamline's `vkQueuePresentKHR` (`VulkanSwapchain::setPresentFunction`, from the interposer's `vkGetDeviceProcAddr`), which manual hooking still needs for its per-frame bookkeeping.
- `renderSize(output, scale)`: DLSS works in its modes (`nearestUpscalingMode`: DLAA, Quality, Balanced, Performance, Ultra performance), each taking render sizes in a range (`slDLSSGetOptimalSettings`, cached; Quality at 2560 x 1440 takes 1280 x 720 to 2560 x 1440, Ultra performance only 853 x 480), so the scale's size is kept inside it (`clampRenderSize`) by `ensureFrameTextures`.
- `dispatch`, each DLSS frame after the camera motion pass (as FSR's): `slGetNewFrameToken` (the jitter frame's number), `slSetConstants` from `makeDlssFrameInputs` (matrices as Streamline stores them, DirectX's row vectors, `streamlineMatrix`, ours transposed: the GPU lens and its inverse, `taaReprojection` and its inverse as clip to previous clip; the jitter in render pixels; `mvecScale` (-0.5, +0.5), making our NDC differences shares of the screen from now to then; the camera's world position and axes, near, far, field of view and aspect; depth inverted, camera motion included, `reset` when DLSS did not draw the last frame), `slDLSSSetOptions` (the mode, the output's size, HDR colour, DLSS's own exposure, NVIDIA's default model per mode), then `slSetTagForFrame` (colour, depth and velocity in `SHADER_READ_ONLY_OPTIMAL`, the output in `GENERAL`, with their sizes, formats and uses; `eValidUntilPresent`, so Streamline reads them where they are rather than copying them) and `slEvaluateFeature`. DLSS records its own barriers and returns the images to those layouts; the output (also `TRANSFER_DST`, which DLSS clears) then goes to `SurfaceRead` and is `finishedScene`. Refused, DLSS becomes unavailable and the frame falls back to the resolve.

**Checked** with the validation layers (no messages) at DLAA, Quality and Ultra performance in the light lab, the demo at Performance, and without the interposer (one log line, TAA). DLSS's jitter convention is NVIDIA's "the same as the motion vectors'": pixels at the render size, x right, y down, as `jitteredClip`'s; DLSS's development DLL has hotkeys that flip it, if a still scene ever shimmers.

### Settings

`TaaSettings` (`RenderSettings::taa`, the TAA group; whether anything temporal runs is `upscaling.upscaler`):

| Setting | Default | Meaning |
|---|---|---|
| `currentWeight` | 0.1 | each new frame's share of the average (about ten frames) |
| `clipSigma` | 1 | the clip box's half-width in the neighbours' standard deviations |
| `jitter` | on | off holds the projection still |
| `showMotion` | off | the motion debug view |
| `upscaling.renderScale` | 1 | the render size's share of the output's (Upscaling) |
| `upscaling.upscaler` | DLSS | the anti-aliasing choice: Off, TAA, FSR 3.1 (FSR) or DLSS (DLSS) |
| `upscaling.sharpness` | 0.8 | FSR's RCAS sharpening, 0 off (FSR) |
| `upscaling.dlssModel` | M | the DLSS model in every mode (M trailed least at the flashlight beam's edge): Default (DLSS picks: K for DLAA), J, K, L or M (`dlssPresetNumber`, Streamline's preset numbers) |

### Golden images

A capture draws `taaSettleFrames` (32, two jitter cycles) before it when anti-aliasing is on, more if probes or the cascades' depth fit need more, starting the history and the jitter afresh, so captures repeat exactly. With anti-aliasing off nothing is jittered or resolved and the picture is as before.

## Screen-space reflections

Glossy surfaces reflect what is on screen (`render/gpu/screen_reflections`, `screen_reflections.glsl`, mirrored; `VulkanReflections`), and what is round the camera through the reflection probe (next section). Until now the specular part of the light from all around was the light probes' irradiance in the mirror direction, a reflection as blurred as the roughest surface's; it stays as the stand-in wherever neither finds anything.

### The main pass's surfaces

With reflections on (and anti-aliasing on, since they reflect last frame's smoothed picture), the main pass writes three more targets at the render size, through outputs 1 to 3 of `triangle_surfaces.frag` (the main pass's shading, `scene_surface.glsl`, with `REFLECTION_SURFACES` defined), so pipelines without them hold none of the code:

| Target | Format | Holds |
|---|---|---|
| Normal | `R16G16_SFLOAT` | the shaded normal, octahedral (`octahedralEncode`) |
| Properties | `R8G8B8A8_UNORM` | the specular albedo, and the roughness (1 without a normal) |
| Stand-in | `B10G11R11_UFLOAT` (RGBA16F where it cannot be drawn into) | the probes' (or flat ambient's) light in the mirror direction times the specular albedo: what the pixel added for its reflection |

They are not cleared; the reflection pass reads only pixels with a surface. The renderer keeps twelve main pipelines (`mainPipelines`, `mainPipelineIndex`: after the prepass or not, probes or not, surfaces or not, and without the prepass cut-outs or not), and `PipelineDescription::extraColourFormats` names the extra targets. The sky's and stars' pipelines draw into the scene image alone, so with the surfaces they are drawn in a second rendering block after the main one, which loads the colour and depth (the depth stored for it).

### The pyramids

- **Depth pyramid** (`depth_pyramid.comp`, R32F, a full mip chain at most 16 levels): level 0 copies the depth image, each level above keeps the nearest depth (in reversed-Z the largest) of the 2x2 texels below, and at the far edge of an odd level also the texel left over (`pyramidLastFine`), so every texel has a cell above it (`pyramidCell` clamps). One dispatch a level, each level made visible to the next by its own barrier.
- **Colour pyramid** (`colour_pyramid.comp`, RGBA16F, the same levels): level 0 is last frame's finished picture (`finishedScene`, still last frame's when the pass runs: TAA's history, FSR's or DLSS's output) resampled to the render size, each level above one bilinear read between the 2x2 below.

### The march

`reflections.comp`, one invocation per render pixel, in place on the scene image (which gained storage use):

1. Skip the sky, pixels at or past `maxRoughness`, and mirror rays into the surface (normal maps can tilt them).
2. The ray starts `SSR_START_OFFSET_PIXELS` pixel footprints off the surface along its normal (`pixelFootprint`: the world size of a pixel per metre, from the view-projection's second row), so the surface it leaves does not hide it.
3. `screenRay` projects it: pixel x and y and reversed-Z depth are all linear in t on screen (depth is affine in 1/w, as x/w and y/w are), so it is cut like a box by the screen's edges and the depth range, after its far end is cut to `SSR_NEAREST_W` in front of the camera.
4. `hizStep` marches through the pyramid (McGuire and Mara 2014, Uludag 2014): in front of a cell's nearest depth the ray skips to where it reaches that depth (a level finer) or out of the cell (a level coarser); at or behind it, a level finer without moving; below level 0 it has hit. A hit more than `thickness` behind the surface (in view distance) is something the ray passes behind, and the march goes on (`passTexel`). Up to `maxSteps` steps (`traceScreenRay` is the loop's CPU mirror).
5. The hit's surface is found in last frame's picture by its motion if it moved, else through the two cameras, from its centre without this frame's jitter (`reprojectPixel`, as TAA).
6. The light is read from the colour pyramid at `reflectionConeLevel`: the reflected cone's half angle has tangent `SSR_CONE_SPREAD` alpha (GGX's lobe at half its peak, the mirror turning twice as far), and the level is log2 of its width after the pixels travelled.
7. `reflectionConfidence` multiplies five fades: roughness (full to `sharpRoughness`, none from `maxRoughness`), the screen's edges (`edgeFade` of the half-width), the reach's last fifth, hits on a surface met from behind (none past a cosine of -0.2), and rays heading back towards the camera (full up to a cosine of 0.3 with the way back to it, none from 0.7: `SSR_TOWARDS_CAMERA_START`, `_END`). Those can only really hit what lies between the surface and the camera, yet pass just behind surfaces near the camera on screen and count as hits within the thickness: the light lab's mirror seen head-on showed a wedge of the window from the other side of the room. A mirror seen head-on is left to the probe or ray tracing.
8. `reflectedScene`: scene + confidence (specular albedo x light - stand-in), never below 0. The probe's light goes in first, by its own confidence, and its blend with the stand-in (`blendedStandIn`) is what this light then replaces. Where neither is trusted at all the pixel is not written, so it is exactly the main pass's.

Reflections in reflections build up a bounce a frame, since last frame's picture holds last frame's reflections. The pass runs only when last frame's picture is usable (`previousPictureUsable`: last frame was smoothed and nothing was remade since).

### Settings

`ReflectionSettings` (`RenderSettings::reflections`, the Reflections group):

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | on | off: no surfaces, no pass (Low turns it off) |
| `sharpRoughness` | 0.3 | trusted fully up to here |
| `maxRoughness` | 0.6 | not at all from here (the stand-in) |
| `thickness` | 0.3 m | how far behind a surface a ray may be and still hit it |
| `reach` | 1000 m | how far a ray goes (hits far away are as good as near ones on screen; at 100 m the light lab's far towers fell to the probe) |
| `maxSteps` | 64 | steps through the pyramid (Medium 32) |
| `edgeFade` | 0.1 | the share of the half-width near the screen's edges where hits fade |
| `screenSpace` | on | screen-space reflections (off: the probe alone) |
| `probe` | on | the reflection probe (off: screen-space reflections alone) |
| `probeSize` | 256 | the probe's faces in texels (High 128, Medium 64) |
| `probeFar` | 300 m | how far the probe's capture sees |
| `showConfidence` | off | the debug view: white where screen-space reflections are trusted, blue where the probe is |

Timed as SSR (`GpuPass::Reflections`), between the probe's capture and the glass (GLASS, then anti-aliasing). The light lab's reflection corner (`labPolishedSlab`, `labMirrorBallCentre`) and the sunlit room's mirror facing the doorway (`labMirror`), with a brushed steel panel beside it (`labSteelPanel`, roughness 0.2: a rough mirror for mirrored lights' footprints), are the places to judge them.

## Reflection probe

One 360-degree picture taken where the camera is, every frame reflections run, for what the screen cannot reflect (`render/gpu/reflection_probe`, `reflection_probe.glsl`, mirrored; `VulkanReflectionProbe`). Placed automatically: no boxes to place per level.

### The cube

Six faces in Vulkan's cube order and addressing (+X, -X, +Y, -Y, +Z, -Z; `cubeFaceAxes`, `cubeTexelOf`, `cubeTexelDirection`), so shaders read it as a `samplerCube`. A face's camera has rows right, up and minus forward with a square 90-degree lens (`cubeFaceViewProjection`, reversed-Z from `PROBE_NEAR` 0.05 m to `probeFar`): the cube's faces are mirror images of an ordinary camera's, so the capture culls front faces.

| Image | Format | Holds |
|---|---|---|
| Capture | RGBA16F cube, full mips | the light each face sees (its mips for the prefilter) |
| Depth | D32 array of 6 | each face's depth |
| Distances | R32F cube | how far from the probe each texel's surface is (`probeRadialDistance`); the far plane for the sky |
| Light | RGBA16F cube, `probeLevels` (down to 4 texels) | the light prefiltered for a roughness per level, 0 to 1 |

### The capture

`drawProbeCapture`, timed as CUBE (`GpuPass::ReflectionProbe`), after the depth measurement:

1. Each face's draws are culled against its own frustum and listed after the velocity pass's in the frame's draw list.
2. Each face is drawn with the main pass's shading: `triangle.vert` takes the face's view-projection from push constants (specialization constant `PROBE_CAPTURE_VERTEX_CONSTANT`), and `probe_capture.frag` (`scene_surface.glsl` with `PROBE_CAPTURE`) shades every light rather than the clusters' lists (those are the camera's) with no heatmap. The sky is drawn over each face's background (`packCubeFaceRays`); the stars are not.
3. The sun's cascades are fitted to the camera's view, so the capture sees surfaces they do not cover, which would be lit through walls and roofs. On GPUs with ray queries `probe_capture_rays.frag` traces the sun's shadow there instead (`sunBeyondCascades`, `reachesLight`, shared with the light probes' trace; the ray tracing scene is built for it), in a layout with the ray tracing set; without them it counts as lit, as beyond the shadow distance.
4. `finishCapture`: the capture's mips (`generateMipmaps`, shared with texture uploads), the distances (`probe_distance.comp`), and the prefiltered levels (`probe_prefilter.comp`, `prefilterTexel`): each texel averages 64 GGX samples round its direction for its level's roughness (Karis's split sum: normal and view along the direction), each read from the capture's level as wide as its share of the lobe (`prefilterSourceLevel`, filtered importance sampling), weighted by n.l. The GGX samples (`ggxSampleHalfway`, `radicalInverse`, `alongNormal`) are the BRDF table's.

### The parallax search

A picture from one point is wrong seen from another. For a pixel at P (measured from the probe) and its mirror direction R, the ray P + R t meets a surface where it first goes behind what the probe saw in the direction of its own point (distance impostors, Szirmay-Kalos et al. 2005), `parallaxMismatch` = |P + R t| - D(direction) going from negative to positive.

- **Lift:** the ray starts `probeStartLift` off its surface: `PROBE_START_LIFT_TEXELS` (2) of the probe's texels at the point's distance, over the cosine between the normal and the way to the probe (at least `PROBE_START_LIFT_SLANT`, 0.25). Unlifted, rays rising from the slab caught on the ground's coarse texels just past their start, in alternate rows.
- **Leaving:** from `PROBE_MARCH_START` (2 cm), doubling up to `PROBE_LEAVE_STEPS` (8) times until the ray is in front of what the probe saw.
- **Distance pyramid:** the distances cube has a full mip chain, each level the nearest of the 2x2 below (`probe_distance_min.comp`, `buildProbeDistanceCube` on the CPU). It is read three ways: a level's exact texel (nearest-filter sampler reaching every level), level 0 blended between texels (so a floor seen at a slant reads as continuous), and level 0's four texels round a direction (`textureGather`, `probeDistanceSpread`).
- **Walk:** at most `PROBE_MARCH_ITERATIONS` (64) steps, cell by cell through the pyramid, as screen-space reflections walk theirs. A cell's edges are planes through the probe, so where the ray leaves one is one division per edge (`cubeCellOf`, `cubeCellExit`). If the ray stays farther than the cell's nearest distance across it (its distance from the probe along a line is greatest at an end), it goes on to the next cell, a level up; else a level down without moving; at level 0 the blended distance where it leaves the texel says whether it went behind. A ray far behind the texel's own surface yet in front where it leaves has passed behind something thin the probe saw in front of it (a wall seen edge-on), and returns nothing. The first version marched in fixed steps and stepped over such walls; the second checked whole cones of directions against wide pyramid levels and crawled near floors seen at a slant, running out of steps in bands.
- **Early stop:** once the probe's line of sight to the ray is within a texel of the ray's own direction (it turns towards it as the ray goes on), the probe sees what the rest of the ray sees, and the light is read along the ray. A ray past the far plane has escaped the same way.
- **Halving:** `PROBE_REFINE_STEPS` (8) halvings of the texel's stretch of the ray where it went behind, 256 times narrower, on the blended distances.
- **Confidence:** the larger of two. `parallaxConfidence` (the blended distances either side of the crossing agree: fully within 3%, not at all past 15%) times `probeEdgeConfidence` (the four texels blended there spread less than 20% to 40%: one surface, not an edge between two). Or `probeViewConfidence`: the probe's direction to the found point within one texel of the ray's own (fully) to two (not at all), where the probe sees what the ray sees, edges or not; far things seen from near the probe are trusted this way across their silhouettes.
- **Why not solve the equation directly:** |P + R t| = D has other roots further along the ray, through doorways and behind walls. The first version iterated towards a root and could settle on one of those: the light lab's mirror showed the back room through the wall beside the doorway, the doorway pinched towards the floor, and curved cuts followed the sphere |P + R t| = D round the probe. Tested now with a doorway the ray passes beside, through and across the edge of (`reflection_probe_test`). A march of fixed geometric steps then still stepped over a wall the probe saw edge-on from a doorway (a sliver on the cube), finding the room beyond: hence the pyramid, tested on a 256-texel cube of a doorway scene.

### Ray-traced reflections

On GPUs with ray queries, `ReflectionSettings::rayTraced` (on by default: the Ultra preset's, off below it) runs `reflections_rays.comp` (`reflections_pass.glsl` with `REFLECTION_RAYS`) in place of the probe's search: each glossy pixel's mirror ray is traced through the scene, glowing stand-ins for lights included (mask 0xFF), as far as `probeFar`.

- A miss brings back the sky: the frame's background plus the atmosphere's light in that direction.
- A back face brings back nothing.
- A front face brings back the probe's picture of that point (its direction from the probe, at the level for the roughness) as far as the probe saw it there (`parallaxConfidence` of the probe's distance to the hit and the stored one), and elsewhere the hit lit as the light probes' rays light theirs (`ray_hit_lighting.glsl`, shared with `ddgi_trace.comp`: matte, direct light by shadow rays, last frame's probe light in full, or the flat ambient without probes), blended by that confidence.

The pass's own set comes after the scene's two sets and the ray tracing set (`ComputePipelineDescription::sharedSets`), which the renderer binds (`ReflectionRaySets`); the ray tracing scene is built for it. Screen-space reflections still go on top.

The light is read in the found direction at the level for the pixel's roughness (`probeRoughnessLevel`), faded by the same roughness fade as screen-space reflections.

## Mirrored lights

Light that reaches a surface by way of one reflection off a shiny one: a flashlight shone into a mirror lighting the floor it points at (a caustic). Direct lighting only follows straight lines, and the light probes' rays see a metal mirror as black (it has no matte colour), so neither carries it. `render/gpu/mirrored_light` (mirrored by `mirrored_light.glsl`, numbers in `mirrored_light_shared.h`), the search with its rays in `mirrored_light_search.glsl`, `render/vulkan/vulkan_mirrored_light` and `mirrored_light.comp`.

**Who runs the search** (`mirroredLightAt`, all four over the same pairs):

| Where | What it lights | Footprint samples |
|---|---|---|
| `mirrored_light.comp` (the pass) | each pixel of the view, for the main pass | `MIRROR_FOOTPRINT_SAMPLES` (4) |
| `ddgi_mirror.comp` (after the probe trace) | the probes' fast rays' hits, into the fast atlas: the lit spot bounces | `MIRROR_HIT_FOOTPRINT_SAMPLES` (1) |
| `probe_capture_rays.frag` | the reflection probe's traced capture: reflections show the lit spot | 1 |
| `reflections_rays.comp` | traced reflections' hits | 1 |

**Settings** (`MirroredLightSettings`, `RenderSettings::mirroredLights`, F5 Reflections):

| Setting | Default | |
|---|---|---|
| `enabled` | on | on GPUs with ray queries, with the depth prepass on; the Ultra preset's, off below it |
| `bounce` | on | the light probes bounce it on (`ddgi_mirror.comp`) |
| `mirrorRoughness` | 0.25 | surfaces up to this roughness reflect lights, fading out over `MIRROR_ROUGHNESS_FADE` (0.05) more (`mirrorRoughnessFade`); past `MIRROR_FOOTPRINT_ROUGHNESS` (0.1) from a footprint |

**Pairs** (`chooseMirrorPairs`, CPU, each frame): each fast light (`GpuLights::fastPoints`, `fastSpots`: the flashlight, the demo's flying lamps) with each shadow-casting draw not past `mirrorRoughness` and its fade whose bounding sphere its range (and a spotlight's cone, `coneTouchesSphere`) reaches, nearest first by the gap between the light and the sphere, at most `MIRROR_MAX_PAIRS` (16). The surfaces are found from their materials: nothing is flagged. Without pairs the pass does not run and the frame is drawn exactly as before. They live in scene set 1 (binding 24, `GpuMirrorPairs`: their count, the roughness limit, then each pair's light kind and slot, draw and bounds measured from the camera; a count of 0 when off), so every user reads the same ones.

**The search** (manifold next event estimation, Hanika, Droske and Fascione 2015), per pixel and pair, from the pixel's point x (from the prepass's depth) to the light L by way of a point y on the surface:

- The law of reflection at y: the half vector h = unit(x − y) + unit(L − y) lies along the normal, so its components along the tangents (`tangentFrame`, Duff et al. 2017) are zero: the constraint c(y) (`mirrorConstraint`).
- Its Jacobian over moves d in the tangent plane (`mirrorJacobian`): J d = −T P d − (h·n) S d, with P = (I − ωx ωxᵀ)/dx + (I − ωL ωLᵀ)/dL from the two unit vectors and S the surface's shape operator (dn = S dy). S comes from the hit triangle's corners (`triangleCurvature`): the interpolated normal changes linearly over the triangle, so its change along each tangent follows from the edges and the corner normals' differences (least squares through the edges' Gram matrix), over the interpolated normal's length; zero for a flat triangle, about 1/R on a sphere mesh.
- The steps (`solveMirrorPath`, the loop in `mirrored_light_search.glsl`): a ray from x (lifted towards the camera by `MIRROR_RAY_OFFSET` + `MIRROR_RAY_OFFSET_PER_METRE` per metre) towards the pair's bounding sphere's centre puts the first guess on the surface. It must hit the pair's draw first, from the front (mask `RAY_MASK_SOLID`), which also proves the way from x clear. Then the flat mirror step (`planeMirrorPoint`: where the line to L's image behind the tangent plane crosses it, exact for a flat mirror). On a flat surface (`isFlatMirror`: curvature under `MIRROR_FLAT_CURVATURE`) a step that does not land on the surface, or lands on a face not in its plane (normals further apart than `MIRROR_SAME_PLANE_COSINE`, 0.999: a thin mirror's rim), ends the search off the mirror (`MirrorPath::onMirror` false): the point stays on the mirror's plane, where a sharp mirror gives no light and a rough one's footprint may still reach the mirror. A receiver on the plane itself (within `MIRROR_RAY_OFFSET`: the mirror's own surface) gets none. Without that early end, pixels whose reflection point lies off the mirror's edge crept towards the edge for up to 40 rays, and the pass took 3.2 ms instead of 0.7. Then Newton steps (`mirrorNewtonTarget`: y − J⁻¹ c, cast back onto the surface by a ray from x), each halved up to `MIRROR_BACKTRACKS` (4) times while it falls off, at most `MIRROR_MAX_STEPS` (8), until |c| is within `MIRROR_SOLVE_TOLERANCE` (0.001), with x and L both in front of the surface.
- A shadow ray from y (lifted along its normal) to the light, after the spotlight's cone there (`lightReachesMirror`).

**The brightness**: the light's intensity I sends I dΩ into the solid angle round its ray to y, landing on an area dA at x across the ray, so the irradiance there is I dΩ/dA. By implicit differentiation of the constraint, dA/dΩ = dx² dL² |det J| / cos²θ: the spread area (`mirrorSpreadArea`). For a flat mirror it is (dx + dL)², the light's image's squared distance; head-on off a ball of radius R it is (dx + dL + 2 dx dL/R)² (the mirror equation); a concave mirror focuses it to 0. It takes the place of distance² in the lights' falloff (`mirroredIrradiance`): I × cone × Fresnel × `rangeWindow`(dx + dL)² / (area + sourceRadius²), so a flat mirror gives exactly the light's image with the engine's falloff, and a focus stays finite (a surface cannot be lit brighter than by the source filling its sky). The reflectance is Schlick's Fresnel from the mirror's material at the hit (base colour and metallic, read as the main pass reads them) at the angle of incidence, times the roughness fade.

**Rough mirrors** (past `MIRROR_FOOTPRINT_ROUGHNESS`, 0.1; brushed steel, polished stone): the facets round y are tilted by the roughness, so a patch round it, its footprint, sends the light on to x.

- **The integral.** The microfacet BRDF D F G / (4 cos θi cos θo) turns the light reaching x into I F G / (4 dx² dL²) times the integral over the mirror of D at each point's half vector (the cosines cancel against the irradiance at y and the solid angle at x). With D a Gaussian in slope space (Beckmann's, the same alpha = roughness² as GGX, so the same peak), and the half vector's slope J d / a for a step d from y (a = h·n), the integrand is a Gaussian on the mirror with covariance s² (JᵀJ)⁻¹, s² = a² alpha² / 2 + (emitterRadius / dL)² / 4 (the facets, and the bulb's size: `emitterRadius`, as for soft shadows, not `sourceRadius`, the falloff's dial, which the flashlight sets to 4.5 m and which spread the footprint over the whole mirror).
- **On a mirror bigger than the footprint the integral is exactly the sharp answer.** A rough mirror does not dim the reflected spot, it softens its edges: the mirror's own, a spotlight's cone, shadows.
- **The estimate** (importance sampling): `MIRROR_FOOTPRINT_SAMPLES` points each frame from a Gaussian of precision JᵀJ / s² + I / R², the true footprint held to the mirror's size R (its bounding sphere), so a focus's footprint (det J = 0, infinitely long one way) stays on the mirror and its light finite. `gaussianSamplePoint`: stratified radii sqrt(−2 ln(1 − u)), angles spread evenly, turned each frame by interleaved gradient noise of the pixel and the frame (`mirroredLightOptions.y`, the TAA frame mod 1024), so every sample is exactly normal over the frames. Each point is put on the mirror by a ray from x (`projectOntoMirror`, which also proves it visible), lit by `lightReachesMirror`, and weighted by the true density over the drawn one, `footprintWeight` = 1 / (s² sqrt(det P)) exp(|d|² / 2R²), 1 / |det J| everywhere on a big mirror (`mirrorFootprint` computes det P as det(J)² / s⁴ + tr(JᵀJ) / (s² R²) + 1 / R⁴, not from the Cholesky factor's last entry, which a near-singular J would cancel away). The light: I F `rangeWindow`² a² / (4 dx² dL²) times the mean weight (`roughMirroredIrradiance`). Temporal anti-aliasing averages the frames.
- **Off the mirror.** A receiver whose point of reflection lies past a flat mirror's edge keeps it on the mirror's plane (`onMirror` false), so the soft edge reaches outside the sharp one.
- **What it drops.** GGX's long tail (Gaussian slopes), the facets' shadowing G, and the change of F, the distances and the slope's 1 / cos⁴ across the footprint: under 2% at the roughness it is used for, by the test against the exact sum.

**The pass** (`VulkanMirroredLights::light`, timed as MIRROR, between the depth prepass and the main pass): one invocation per render pixel, `MIRROR_GROUP_SIDE` (8) square, the search from each pixel's point lifted towards the camera. Its own set 3 after the scene's sets and the ray tracing set: the pass record (`GpuMirroredLightPass`: the camera-relative view-projection's inverse with the jitter, and the size), the depth (exact texels), and two render-size images: the light arriving (RGBA16F) and its direction folded onto a square (RG16F, `octahedralEncode`), the pairs' directions blended by brightness (exact for one pair). Barriers: the depth from the prepass to compute reading (the main pass turns it back into a depth target, kept); the images from last frame's fragment reads to storage writes, contents discarded, then to fragment reads. The images are made the first time the pass runs and remade with the frame's size after that.

**The main pass** reads them (set 1 bindings 22 and 23, `MirroredLightBinding`, `MirroredDirectionBinding`; the probe stand-in texture while the pass does not run) at its own pixel while `mirroredLightOptions.x` is 1 (not in the reflection probe's capture), and adds the light with `addLight` from the stored direction, so diffuse and glossy surfaces both respond (a glossy floor shows the mirrored flashlight's highlight).

**The bounce** (`ddgi_mirror.comp`, in the probes' update right after the trace, while the frame has pairs, a listed fast light and `bounce` on): the trace's rays again, its layout, sets and push constant. Only rays of probes a fast light reaches (`probeNearFastLight`, which the trace wrote; every pair's light is fast) that met a front face (the trace's distance) are traced again to find their surface; the mirrored light on it (`mirroredLightAtRayHit`, from the hit lifted by `DDGI_SHADOW_RAY_OFFSET`, times the cosine to its blended direction) on the matte colour joins the fast lights' light in `fastProbeRays`. Barriers: the ray buffers from the trace's writes to its reads and writes, then on to the update's reads. A pass of its own: inside the trace, the search's registers slowed every probe ray from 5.7 to 8.3 ms (2560 x 1440, Ultra), run or not; `ray_hit_lighting.glsl` compiles `mirroredLightAtRayHit` only where `RAY_HIT_MIRRORED_LIGHTS` is defined (here and in the traced reflections).

**In reflections**: the reflection probe's traced capture (`probe_capture_rays.frag`) runs the search at each texel, from the point lifted along its normal, and adds it as the main pass adds the pass's light; traced reflections' hits add `mirroredLightAtRayHit` to their light. Screen-space reflections already show it where it is on screen.

**Metals' matte colour** (`matteColour` in `brdf`: base colour × (1 − metallic)): the rays' hits (`rayHitSurface`, the probes and traced reflections) are lit matte only, and took the base colour whole, so the light lab's mirror looked like a white wall and the chrome ball like plaster to them; they now take the matte colour the main pass's diffuse part is built from (`prepareShading` uses the same function, bit for bit as before). The light near metals in the probes changes.

**Tests**: `mirrored_light_test` (a flat mirror is exactly the image light; off the mirror's edge (a point on its plane) and its rim, behind it, and the flat early exit after two rays; the ball's point obeys the law of reflection and its Jacobian matches finite differences; the head-on ball's spread area; and the spread area checked against the light itself, 2.25 million rays from the light reflected off an exact sphere and counted landing round the receiver, which agreed within 0.3%; rough mirrors: the samples are standard normal over the frames, the footprint's covariance and weights, a rough flat mirror and a rough ball give the sharp light away from their edges and a tiny roughness gives it in one frame, five receivers across a finite rough mirror's reflected edge match the exact sum over its Beckmann facets (a million squares each) within 3% of the centre's light, and a focus stays finite; the roughness fade, and the pairs' buffer); `brdf_test` (`matteColour`); GPU tests in `mirrored_light_gpu_test` (the footprint's functions too) and `brdf_gpu_test`.

**Measured** (RTX 5080, 2560 x 1440, the flashlight shone into the light lab's mirror, about a third of the screen lit by it): 0.68 ms at first. With the flashlight shone down into the mirror from the back room's doorway (the floor and window wall lit by it, Ultra), 60-frame averages:

| Pass | Off | Sharp mirror | Mirror at roughness 0.2 |
|---|---|---|---|
| Mirrored light pass | 0 | 1.21 ms | 2.34 ms |
| Probes (with the bounce) | 5.92 ms | 7.96 ms | 7.45 ms |
| Reflection probe capture | 0.58 ms | 0.91 ms | 0.92 ms |

The bounce scales with the fast rays a probe traces (measured at 1024; 256 by default since `ultra-rt-defaults`, about a quarter of the cost). **Not yet**: the image light for flat mirrors without ray tracing.

## Ambient occlusion

How much of the light from all around reaches a point past the small things next to it: creases, the foot of a wall, the gap under a block. The light probes give the light from all around and their visibility catches walls and rooms, but they sit 0.5 m apart at best; ambient occlusion supplies the visibility below that. `render/gpu/ambient_occlusion` (mirrored by `ambient_occlusion.glsl`, numbers in `ambient_occlusion_shared.h`), `render/vulkan/vulkan_ambient_occlusion`, `ambient_occlusion.comp` and `ambient_occlusion_blur.comp`.

**Settings** (`AmbientOcclusionSettings`, `RenderSettings::ambientOcclusion`, F5 Ambient occlusion):

| Setting | Default | |
|---|---|---|
| `enabled` | on | needs the depth prepass; off on the Low preset |
| `radius` | 0.5 m | how far occluders count: about the densest probes' spacing, which handle what is bigger |
| `falloff` | 0.6 | the outer share of the radius over which an occluder's weight fades to nothing (`occluderWeight`), so the reach has no hard border |
| `slices`, `steps` | 3, 6 | slices a pixel each frame, steps along each side (presets: High 2 and 4, Medium 2 and 3; at most `AO_MAX_SLICES` 8 and `AO_MAX_STEPS` 16) |
| `multipleBounces` | on | a crease's walls bounce light back in (`multiBounceOcclusion`) |
| `specularOcclusion` | on | the reflection dims in creases too (`specularOcclusion`) |

**The method** (ground-truth ambient occlusion, Jimenez, Wu, Pesce and Jarabo 2016), per pixel of the render size, from the prepass's depth alone:

- **The point and its normal.** The point from the depth (`positionAtPixel`, camera-relative). The normal from the steps to the neighbours across and down (`depthNormal`): each way, of the two neighbours, the one whose next neighbour carries on in line with it (`surfaceStep`; a straight line of pixels across a plane is a straight line in space). Taking the nearer neighbour instead failed at a wall's foot, where the wall's pixel above is nearer than the foreshortened floor's below: the floor took the wall's normal, and the test's long-radius occlusion there came out 0.62 instead of 0.5. A neighbour off screen or on the sky counts as the pixel itself and gives way.
- **Slices.** A slice is the plane through the point holding the way to the camera, turned round it by an angle; on screen, a line through the pixel. Its angles are spread over half a turn and turned by interleaved gradient noise of the pixel and the frame (the TAA frame mod 1024; still while there is no anti-aliasing), so the frames average many.
- **Horizons.** Each side walks `steps` texels along the line (`aoSampleDistance`: quadratic in the step, dense near the pixel, staggered by noise, never under `AO_MIN_SAMPLE_PIXELS` 1.5), out to the radius on screen (`aoScreenRadius`, from the pixel footprint per metre and the view distance, at most `AO_MAX_SCREEN_RADIUS` 256). Each sample's point gives the cosine of its angle from the way to the camera (`horizonCosine`), pulled towards the surface's own horizon (`lowHorizonCosine`: the normal's angle plus or minus a right angle) by its falloff weight; each side keeps the highest. Samples off screen or on the sky count as open.
- **The open arc.** With the normal projected into the slice (`aoSlice`: angle n from the way to the camera, length l), the cosine-weighted open share between the horizons h0 and h1 is l times the integral of cos(θ − n) |sin θ| from h0 to h1, each side (cos n + 2h sin n − cos(2h − n)) / 4, the horizons kept within a right angle of the normal (`sliceVisibility`). Averaged over slices round the way to the camera this is the cosine-weighted visibility exactly, 1 for an open surface whatever its tilt (tested over 2000 slices).
- **The blur** (`ambient_occlusion_blur.comp`, only without anti-aliasing): the mean over 5 x 5 texels (`AO_BLUR_RADIUS` 2), each weighted by how near its view distance is to the pixel's (`aoBlurWeight`, nothing past `AO_BLUR_DEPTH_SHARE` 10%), so it does not bleed across edges.

**The main pass** (set 1 binding 25, `AmbientOcclusionBinding`, while `ambientOcclusionOptions.x` is 1; not in the reflection probe's capture) reads the visibility at its pixel, takes the more closed of it and the material's baked occlusion where it has one (`surfaceVisibility`: min(screen, 1 + strength (texel − 1)), glTF's formula; the minimum, not the product, since both estimate the same open share and a crease both see would darken twice; the baked one also in the reflection probe's capture, being the surface's own) as v, and dims only the light from all around (the probes' or the flat ambient's), not the direct light, which has its shadows:

- the diffuse part by `multiBounceOcclusion(v, matte colour)` (Jimenez's fit: max(v, ((a v + b) v + c) v) with a, b, c lines in the albedo, `AO_BOUNCE_*`), or v alone with `multipleBounces` off;
- the reflection's stand-in by `specularOcclusion(n·v, v, roughness)` (Lagarde and de Rousiers 2014: (n·v + v)^(2^(−16 r − 1)) − 1 + v, clamped), and the specular albedo it hands the reflection pass by the same, so the better light the reflection pass swaps in stays dimmed.

With it off and no baked occlusion every factor is exactly 1 and the picture is as before, bit for bit (the block runs only with the screen's occlusion on or a strength above 0). Ray hits (probe rays, traced reflections) leave baked occlusion out: a small effect on lighting that coarse, for a texture read per ray.

**The pass** (`VulkanAmbientOcclusion::occlude`, timed as AO, right after the depth prepass, before the mirrored lights): `AO_GROUP_SIDE` (8) square groups. Its own set 0: the pass record (`GpuAmbientOcclusionPass`: the camera-relative view-projection's inverse with the jitter, the size, the pixel footprint per metre, the radius, falloff, slices and steps, and the frame), the depth (exact texels) and an R8 image; the blur adds the noisy image and writes a second one. Barriers: the depth from the prepass to compute reading; the images from last frame's reads to storage writes, contents discarded; the noisy image to compute reading for the blur; the one the main pass reads to fragment reading. The images are made the first time the pass runs and remade with the frame's size.

**Tests**: `ambient_occlusion_test` (the closed form against quadrature; an open surface averages exactly 1 tilted or not; half the sky blocked gives a half; the normal at a wall's foot keeps the floor's slope; the screen radius, steps, falloff, bounces, specular occlusion, the baked occlusion's minimum and blur weight; and the pass's loop through a drawn depth image of a floor and a 2 m wall: open floor 1, a metre from the wall over 0.98, the foot under 0.85 with the default radius and 0.506 against 0.5 with a long one, the sky open), `quality_preset_test`, `gpu_timing_test`; GPU tests in `ambient_occlusion_gpu_test` (written and built, not run).

**Not measured yet**: its cost and its look in the game. **Not yet**: half resolution, if the cost calls for it.

## Depth and camera-relative rendering

### Reversed-Z

Reversed-Z (`render/gpu/gpu_depth_range`): `toGpuDepthRange(projection)` turns OpenGL clip depth (-w..w, what `Mat4::perspective` and `Mat4::orthographic` give) into the GPU's reversed w..0.

- Correction row:

  depth = 0.5 w - 0.5 z

- So the near plane is stored as `gpuNearDepth` (1) and the far plane as `gpuFarDepth` (0), both from `render/gpu/depth_shared.h` (`GPU_NEAR_DEPTH`, `GPU_FAR_DEPTH`, read by `fullscreen_triangle.vert` too).
- Every GPU projection goes through it (the camera, point and spot shadow faces, the sun's cascade boxes, the debug lines' screen pixels), so the whole renderer shares one convention:
  - depth tests `GREATER` (main and shadow pipelines);
  - the main depth image is cleared to `gpuFarDepth`, and shadow tiles are reset to it;
  - the shadow sampler compares `GREATER_OR_EQUAL`;
  - the blocker search counts stored depths greater than the receiver's;
  - and `linearShadowDepth` is `f n / (n + depth (f - n))`.
- **Why:** perspective depth goes as 1/distance and float values are densest near 0, so with the far plane at 0 the two nearly cancel and precision is close to even over the view. Two surfaces 1 cm apart at 1 km get about 270 floats between their depths; with the far plane at 1 they got the same depth (`gpu_depth_range_test`).
- `Frustum::fromClipMatrix` takes `ClipDepth::OneToZero` for GPU matrices (planes 4 and 5 stay near and far).
- The far plane stays finite (an infinite one would need a separate draw distance for culling and shadow fitting).

### Camera-relative rendering

Camera-relative rendering (`render/gpu/camera_relative`): everything the main pass's shaders see is measured from the camera along the world's axes.

**How:**

- `cameraRelativeViewProjection(camera)` is the camera's view-projection with the camera moved to the origin (rotation only).
- `triangle.vert` subtracts the camera position (`GpuFrameData::cameraOrigin`) from each instance's world model matrix's translation before any product, which is exactly `relativeTo(model, cameraOrigin)` (checked in `gpu_scene_test`), then multiplies by the relative view-projection. The normal matrix is unchanged.
- `triangle.vert` outputs `relativePosition`, and `packLighting` subtracts the camera from point and spot positions.
- Each `ShadowTileData::matrix` is the tile's view for camera-relative points:
  - point and spot faces are built again from `position - camera` (one subtraction on positions, before any matrix);
  - each cascade box is its world matrix times `translation(camera)` (it is snapped to texels in world space, so it must not be rebuilt around the camera).

**What stays in world space:**

- The shadow pass itself still draws with the world matrices in `ShadowTile::matrix`, because the shadow cache recognises tiles by their matrix and casters: camera-relative drawing would redraw every cached tile whenever the camera moved.
- Culling, light priority and the debug lines (`VulkanRenderer::viewProjection`, next to `relativeViewProjection` and `cameraOrigin`).

**Why:** 12 km from the origin a float's steps are about 1 mm, and multiplying a view matrix holding -camera by a model matrix holding +object subtracts two such rounded numbers.

- Measured there, the world-space path missed the exact screen position by about a quarter of a pixel on a 1920-pixel-wide screen (visible as shimmer), the relative path by about 20,000 times less (`camera_relative_test` asserts at least 20).
- Positions are still floats in the `World`, so phase 13 moves the subtraction to doubles.

## Assets and materials

### Assets

Assets: `MeshHandle` and `TextureHandle` (`scene/asset_handles.h`) are typed indices; a default handle means "none" (no texture = plain colour).

`AssetManager` (`assets/`) owns CPU-side meshes, images and imported models, and gives the same handle for the same shared mesh pointer. It loads each file once:

- `loadMesh` for OBJ, with optional `MeshLoadOptions{.smoothNormals, .creaseAngle}` that are part of the cache key;
- `loadTexture` by path+flip or embedded buffer;
- `loadModel` for glTF, which returns a stable `const Model&`.

Assets are append-only, so handles never go stale (add a generation like `Entity` if unloading is added).

### Materials

**From the loaders.** Loaders produce `Model`s whose parts hold a shared `Mesh` and a `MaterialSource`:

- colour, `metallic`, `roughness`, linear `emissive`;
- `alphaMode`, `alphaCutoff`, `opacity` and `doubleSided` (see Cut-outs and double-sided surfaces), and `transmission` with `transmissionTexture` (see Glass and blended surfaces);
- and five `TextureSource`s, `colourTexture`, `metallicRoughnessTexture`, `normalTexture` (with `normalScale`, glTF's `normalTexture.scale`, default 1), `occlusionTexture` (with `occlusionStrength`, glTF's `occlusionTexture.strength`, default 1) and `emissiveTexture`, each a path or embedded bytes, sharing one flip.

**To a `Material`.** `AssetManager::makeMaterial` turns that into a `Material`:

- colour + `TextureHandle`;
- `metallic` 0 and `roughness` 0.5 by default (the old Blinn-Phong default, shininess 32, matches roughness 0.5);
- an optional `metallicRoughnessTexture` (glTF packing: roughness in green, metallic in blue, multiplying the numbers);
- an optional linear `normalTexture` with `normalScale`;
- an optional linear `occlusionTexture` (baked occlusion in red: how open each texel is to the light from all around, 0 buried to 1 open) with `occlusionStrength` (0..1);
- `emissive`, with an optional sRGB `emissiveTexture` multiplying it (screens, dials, painted lights);
- `alphaMode` (`Opaque` by default, `Mask` or `Blend`), `alphaCutoff` (0.5), `opacity` (1) and `doubleSided` (false), with `isCutOut(material)`;
- and `transmission` (0) with an optional linear `transmissionTexture` (red), with `isTransparent(material)`: blended or transmitting.
- It loads the colour and emissive textures as `TextureEncoding::Srgb` and the metallic-roughness, normal and occlusion ones as `Linear` (`AssetManager::loadTexture(TextureSource, flip, encoding)`). The encoding is part of both texture caches' keys and is stored in `ImageData::encoding`. Occlusion packed into the metallic-roughness image (ORM: occlusion, roughness, metallic in red, green, blue) is the same file or bytes read the same way, so it is one texture.

**The glTF loader** reads `metallicFactor`, `roughnessFactor`, the metallic-roughness texture, the normal texture and its scale, the occlusion texture and its strength (0..1, checked like the factors), the emissive texture, `emissiveFactor`, `KHR_materials_emissive_strength`, the base colour factor's alpha (as `opacity`), `alphaMode` with `alphaCutoff` (at least 0) and `doubleSided` (`readTexture` handles every texture, files and embedded alike). `BLEND` loads as `AlphaMode::Blend`; `KHR_materials_transmission` as `transmission` (0..1) and `transmissionTexture`. Each embedded image is copied once per load (`GltfImages`, keyed by cgltf's image), so slots naming one image share its bytes and the `AssetManager`, which caches embedded images by buffer, decodes and uploads it once. A part with no material gets glTF's defaults, metallic and roughness 1.

**On the GPU.** `packMaterial` (`render/gpu/material_uniforms`) turns a `Material` into `GpuMaterial`, which mirrors the shader's `MaterialData` struct (96 bytes, std430):

- base colour, its alpha a cut-out's or blended material's opacity (1 for opaque materials);
- `surface` = metallic and roughness clamped to 0..1 and the normal scale (0 when there is no normal texture, so the white stand-in tilts nothing), and the occlusion strength (clamped to 0..1, and 0 when there is no occlusion texture, which tells the main pass to leave the light from all around alone);
- emissive (multiplied by the emissive texture in the shaders), w a cut-out's cutoff (0 for opaque materials, keeping every texel);
- `textures`: the slots of the colour, metallic-roughness, normal and emissive textures in the bindless array, `bindlessTextureSlot` = handle + 1, `whiteTextureSlot` 0 for none.
- `extra`: x the occlusion texture's slot, y the flags (`MATERIAL_DOUBLE_SIDED` 1, `MATERIAL_CUT_OUT` 2, `MATERIAL_BLEND` 4, `render/gpu/material_shared.h`)), z the transmission texture's slot, w 0;
- and `transmission`: x thin glass's transmission (0..1), the rest 0.
- One entry per different material in the frame's material buffer (see GPU scene).

### Cut-outs and double-sided surfaces

**What they are.** A cut-out (glTF's `alphaMode: MASK`) is a sheet with holes: leaves, a fence's wire, a grate, drawn as flat polygons whose texture's alpha says where the holes are. A texel is kept, fully opaque, where its alpha (the colour texture's times `opacity`) reaches `alphaCutoff`, else thrown away (`isCutAway`, `render/gpu/alpha_mask`, mirrored in `alpha_mask.glsl`). No blending or sorting: the rest is opaque like any surface. Such sheets are usually double-sided (glTF's `doubleSided`): both sides drawn, the back shaded as the front's mirror image (`facingSign`: its normal and whole tangent frame turned round, so a normal map's bumps stand out of the side seen).

**Keeping their coverage at a distance** (`core/coverage_mips`, Castaño 2010). A smaller mip averages a wire with the holes beside it, so a few levels down everything sinks under the cutoff and fences vanish. A cut-out's colour texture gets its mips built on the CPU when loaded (`coveragePreservingMips`): each level halved from the last one as averaged (`halveImage`: 2 x 2 blocks, an odd edge folded in, sRGB colour averaged as light), then its alpha alone scaled so the share of texels passing the cutoff (`alphaCoverage`) stays the full size's. The scale is found by bisection on a 256-bin alpha histogram: the coverage nearest the target, then of the scales giving it, the one nearest 1 (solid stays solid). The cutoff the texels meet is the material's over its opacity. `AssetManager::loadTexture` and `addTexture` take it as `coverageCutoff`, part of both texture caches' keys (the same file on an opaque material is a texture of its own), into `ImageData::mips`, which `VulkanTexture` copies in instead of blitting.

**Drawing them.** `batchByMesh` sorts each view's draws by style, cut-outs last, and `drawBatches` switches pipeline and cull mode per batch:

| Pass | Plain batches | Cut-out batches |
|---|---|---|
| Depth prepass | no fragment shader | `cut_out.frag` (the colour texture's alpha with the frame's mip bias, `fragmentCutAway` in `cut_out_fragment.glsl`) |
| Main pass after the prepass | as before | the same pipeline: the holes have no depth for its `EQUAL` test to match |
| Main pass without it, the probe capture | as before | the specialization constant `MATERIAL_CUT_OUT_CONSTANT` (1) adds a discard after every texture read |
| Shadow pass | `shadow.vert`, no fragment shader work | `shadow_cut_out.vert` with texture coordinates, `cut_out.frag` |
| Velocity pass | `motion.frag` | its cut-out constant, so what moves behind the holes keeps its motion |

Double-sided batches cull nothing (`vkCmdSetCullMode`); the rest keep the pass's culling (back faces; front faces in the capture, whose faces are mirror images, which also swaps `gl_FrontFacing`, so the shading flips it back).

**Rays.** A cut-out's ray instance is not opaque (`rayInstanceNotOpaque`, Vulkan's `FORCE_NO_OPAQUE`), and every ray query runs through `traceRayQuery` (`ray_scene.glsl`), which confirms a candidate hit only where its texel is solid (`candidateIsSolid`: the full-size texel, as rays have no screen footprint). Opaque instances still commit at once. `committedHitFacesRay` counts either side of a double-sided surface as its front, so probe rays, relocation, mirrored lights and traced reflections never take a leaf for the inside of a wall. These need the material buffer and textures, which every renderer pass with rays binds as sets 0 and 1 (`RAY_SCENE_MATERIALS`); a shader that binds the ray scene elsewhere (the GPU tests') gets neither.

**Tests**: `coverage_mips_test` (halving data and sRGB, odd sizes, coverage, leaves keeping their share where plain averaging loses it, colour untouched, solid and clear unchanged), `alpha_mask_test`, the loader's and asset manager's, `material_uniforms_test`, `gpu_scene_test` (batching by style), `ray_tracing_test` (the flag), `light_lab_test` (the chain-link); GPU tests in `alpha_mask_gpu_test` (written and built, not run).

### Glass and blended surfaces

**Two kinds of seen-through** (`render/gpu/transparency`, mirrored in `transparency.glsl`):

- **Coverage** (glTF's `alphaMode: BLEND`): alpha is the share of the pixel that is the surface, all of it (its reflection too), the rest what lies behind. Right for decals and fading things.
- **Thin glass** (`KHR_materials_transmission`): the whole pixel is glass. Its reflection stays whole (Fresnel: about 4% head-on, nearly all at grazing angles), and of the rest the share `transmission` passes through, tinted by the base colour, in place of the light it would scatter (its diffuse part). Metals pass none. No bending (refraction) yet.

`transparentBlend(alpha, transmission, metallic, diffuse, specular, emissive, baseColour, specularAlbedo)` gives the light added and the transmittance, per channel:

| | Formula |
|---|---|
| passing share | t = transmission x (1 - metallic) |
| surface light | (1 - t) x diffuse + specular + emissive |
| through | baseColour x (1 - specularAlbedo) x t |
| added | alpha x surface light |
| transmittance | alpha x through + (1 - alpha) |

Only `BLEND` materials use their alpha as coverage (`MATERIAL_BLEND`); an opaque glTF material's alpha is ignored, as glTF says.

**Blending** (`drawTransparent`, GpuPass `Transparent`, GLASS): picture = added + transmittance x picture, per channel, by dual-source blending (`BlendMode::DualSourceTransmittance`: the shader's second colour, location 0 index 1, is the destination factor; `dualSrcBlend`, `VulkanDevice::supportsDualSourceBlending`, on every desktop GPU). Without it a grey share: alpha = 1 - the transmittance's mean, premultiplied (`triangle_transparent_grey.frag`).

**Where and in what order.** After the opaque scene, its reflections and the reflection probe's capture, before anti-aliasing (so glass is smoothed too), into the scene image with its depth tested (`GREATER`) and not written (the main pass keeps its depth for this). Transparent draws are left out of every opaque view (`opaqueDraws`, `transparentDraws`) and sorted far to near by their bounds' centre (`sortTransparentDraws`, equal distances in the order chosen), one draw list entry each, after the capture's. A double-sided one is drawn twice from its entry, back faces (front culled) then front faces, so its far side shows through its near side. Whole objects are sorted, so interpenetrating glass can draw in the wrong order.

**Shading.** The main pass's shading (`scene_surface.glsl` with `TRANSPARENT`: `triangle_transparent.frag`), its diffuse and specular parts kept apart for the blend. Its reflection is the reflection probe's prefiltered light in the mirror direction at the roughness's level (`probeRoughnessLevel`; set 2 of `transparentLayout`, `VulkanReflectionProbe::fragmentReadableLight`), while this frame captured one (`GpuTransparentView` push constants, after the vertex shader's 64 bytes); else the light probes' or the flat ambient's mirror light. A one-texel probe stands in when reflections never made one. The screen's ambient occlusion and the mirrored lights' images are the opaque surface's behind, so glass reads neither.

**What glass does not do yet:** cast shadows (`castersInReach` skips it, so the sun passes untinted), stop or tint rays (its ray mask is 0: probe rays, shadow rays, mirrored lights and traced reflections pass through, and `chooseMirrorPairs` skips it), appear in the reflection probe's capture, or write motion (the reactive mask below keeps that from smearing).

**The reactive mask** (`TransparencySettings`, F5 "Transparency": `reactiveMask` on, `reactiveScale` 1, `reactiveLimit` 0.9). Anti-aliasing finds each pixel's history by the motion of what the depth image holds, and glass is not in it, so a glass pixel's history moves as the wall behind it does: moving glass, and reflections sliding over still glass as the camera moves, would smear. The mask says how far to distrust the history at each pixel.

- **Measured, not drawn:** with anti-aliasing on and glass in the frame, `VulkanReactiveMask::keepOpaque` copies the scene image just before the glass pass, and after it `reactive_mask.comp` compares the two (`reactiveShare`, mirrored in `transparency.glsl`) into an R16F image at the render size (not R8: Streamline's Vulkan format table has no 8-bit unsigned normalised single channel, and refuses DLSS's tag with "Cannot have undefined format"). Both run inside the `Transparent` GPU timing.
- **The share:** in each channel, the change as a share of the brighter of before and after (so the same at any exposure, which is not known yet then; `TRANSPARENCY_REACTIVE_FLOOR` keeps black at 0), the most changed channel, times the scale, at most the limit. Clear glass head-on over a bright view changes a few percent; a lit reflection over a dark room, or strongly tinted glass, most of the pixel.
- **Why not a second glass draw:** dual-source blending allows only one colour output, so the glass pass cannot write the mask itself. Comparing the pictures is AMD's own approach (FSR's optional `GenerateReactiveMask`), measures what matters (how much of the pixel is the glass's own light) and would cover particles too.
- **Readers:** FSR's `reactive` input, DLSS's bias towards the current colour (`kBufferTypeBiasCurrentColorHint`), and the engine's resolve (`taa.comp`, set 0 binding 4), where `taaReactiveShare` gives the new frame that share of what the history had, read at the nearest render pixel.
- **Frames without glass,** or with the mask off, clear it to 0 once (`clear`, skipped while it stays clear), so the readers always have one.
- The limit stays under 1 so some history smooths even glass that makes the whole pixel (its edges would otherwise show one frame's aliasing).

**Tests**: `transparency_test` (opaque, coverage, transmission with Fresnel, metals, the split, far-to-near sorting with backs first, unbounded draws, the push constants, the reactive share, its settings and pass), `taa_test` (`taaReactiveShare`), the loader's, asset manager's and packing tests, `culling_test` (no glass shadow casters), `ray_tracing_test` (mask 0), `mirrored_light_test`, `gpu_timing_test`, `light_lab_test` (the window's pane); GPU tests in `transparency_gpu_test` (the blend and the reactive share) and `taa_gpu_test` (`taaReactiveShare`), written and built, not run.

## Input

`Input` is a once-per-frame snapshot with no SDL dependency.

- **Keys:** `isKeyHeld`, `wasKeyPressed`, `wasKeyReleased` take an engine `Key` (`key.h`, physical US-layout positions), plus accumulated `getMouseDelta`.
- **Mouse buttons** (`MouseButton`) work like keys, and each press is also recorded as a `MouseButtonPress` with the exact event position (window pixels) and timestamp (ns, `SDL_GetTicksNS` clock).
  - Use `getLastMouseButtonPress` for clicks, `getCursorPosition`/`isCursorInWindow`/`getWindowSize` for continuous things.
- **Filling it:** through `pressKey`/`releaseKey`/`pressMouseButton`/`releaseMouseButton`/`addMouseMotion`/`setCursor`/`setWindowSize`.
  - `sdl_input` is the only SDL-to-engine translation (`keyFromScancode`, `mouseButtonFromSdl`, `applySdlEvent`, which also drops key repeat).
  - `Window::processEvents(input)` resets and fills it.
- Mouse motion is only forwarded while the mouse is captured, and is discarded when capture or control mode changes.
- Game code reads keys from `Input` as `Key` values, never SDL scancodes or `SDL_GetKeyboardState`.

## Entities and components

Entities and components (`ecs/`): an `Entity` is an index plus a generation (handles to destroyed entities go stale even after their slot is reused; `Entity{}` is never alive and means "none").

- `World` creates/destroys entities and stores one `ComponentStorage<T>` (sparse set: packed `components`, owning `entities`, and a `sparse` index) per component type.
- Components are plain structs; systems are free functions run with `world.each<A, B>([](Entity, A&, B&) { ... })`, which walks the first type's packed array, so list the rarest component first.

**Changes during a walk:**

- Inside `each`, `destroy` and `remove<T>` are deferred:
  - the `World` counts running walks (`activeWalks`, also per storage in `ComponentStorageBase::activeWalks`; they nest, and the const `each` counts too);
  - and queues them as type-erased `PendingChange`s, applied in order by `applyPendingIfIdle` when the outermost walk ends (also when a system throws, so the world never stays locked);
  - until then the entity and its components stay visible.
- `create` is always immediate.
- `add<T>` throws `std::logic_error` while a walk over `T` runs; `addLater<T>` adds at once outside a walk and queues inside one (skipped if the entity is dead by then).
- A queued destroy of a dead entity is a no-op.
- Tests: `deferred_changes_test`.

## Components today

Components today (`ModelRenderer` also has `castsShadows` and `isStatic`):

- `Transform` (`math/`):
  - `position`;
  - `rotation` as a unit `Quat` (`math/quat`, x y z w as glTF; `a * b` turns by b first, then a, like matrices; Euler angles only describe poses, through `Quat::fromEulerAngles`, x first, then y, then z);
  - `scale`;
  - `getMatrix` is translation times rotation times scale, written out directly.
- `PreviousTransform`.
- `scene/`: `ModelRenderer`, `DirectionalLight`, `PointLight`, `SpotLight`, `AmbientLight` and `SkyBackground`.
- `gameplay/`: `Spinner`, `Firefly`, `CharacterMovement`, `FirstPersonBody` and `Grenade`.
- `physics/`: `Collider`, `CharacterController` and `Particle`.

**`ModelRenderer`** is a list of `RenderPart`s (mesh, material, `localTransform` with the model's normalization folded in) plus `visible`; a multi-part model is one entity.

- Parts hold a `MeshHandle` and a `Material`, so the component is small and copyable.
- Build with `assets.makeModelRenderer(model, normalization)` or `makeMeshRenderer(meshHandle, material)`.

## Main loop

Main loop: `FixedTimestep` (120 Hz and frame time clamped to 0.25 s by default, from `EngineSettings`) turns real frame time into a whole number of ticks plus an interpolation `alpha`.

Each frame:

1. `game.onInput` turns input into commands;
2. `runFixedUpdate` runs zero or more ticks;
3. `game.onUpdate` moves the camera;
4. then `render(alpha)`.

Never read `Input` inside a tick; key/click edges are handled once per frame and reach the simulation as commands. Mouse look and camera movement are per frame, not per tick.

## Each tick

Each tick: `savePreviousTransforms` (engine), then `DemoGame::onFixedUpdate` runs, in that order:

1. `updateCharacters`
2. `updateFirstPersonBodies`
3. `updateSpinners`
4. `updateFireflies`
5. `moveParticles`
6. `updateGrenades`

- Spinners and fireflies compute their pose from the total simulation time, so no error builds up.
- A `Spinner` keeps Euler `initialAngles` and per-axis `speed` and sets the whole rotation with `Quat::fromEulerAngles`.
- `DemoGame` keeps `latestSimulationSeconds` so lamps created between ticks start mid-flight.

### Determinism

Determinism: the same build given the same commands reaches a bit-identical state.

- `hashSimulationState(world)` (`engine/state_hash`) hashes every component ticks change field by field with `Fnv1aHash` (`core/hash.h`, shared with `shadowCasterSignature`): Transform, PreviousTransform, CharacterController, CharacterMovement with `MoveToController::getTarget` and `yaw`, FirstPersonBody, Particle, Grenade.
- `determinism_test` plays a scripted demo (walking, jumps, grenades, the lamp field, MOBA right-click) in two worlds and compares the hash every frame, plus a third world one grenade throw short that must differ.
- The same for the light lab (walking with the sun's clock running, jumps, a drop in from the free camera; the one-jump-short world is compared before the drop, which puts every run's body in the same place).
- A new component that ticks change must be added to the hash.

### Interpolation

- Interpolation: only entities that move continuously get a `PreviousTransform`.
- `getRenderTransform(world, entity, alpha)` blends it with `Transform` (`interpolate` lerps position and scale and `slerp`s the rotation, the short way round), and entities without one are drawn at their `Transform`.
- Things that teleport (the destination marker) therefore have no `PreviousTransform`.
- Create moving entities with `PreviousTransform` equal to their starting `Transform`.

## Cameras

Cameras: `Camera` (`scene/`) is a plain lens: position, forward, up, vertical FOV, aspect, near/far. View and projection matrices are built on request, so `setVerticalFov` works (aim-down-sights zoom). It has no movement logic.

Controllers in `gameplay/camera/` move it, each a concrete class with no shared interface (a real game uses one):

- `FreeFlyCameraController`: look + fly, Space/Ctrl up/down; holding Shift multiplies its speed by `LookSettings::sprintMultiplier`, 2.
  - It uses `look_controls`' shared `turnCamera` and `moveCamera`, and the first-person controller only `turnCamera`: walking, and its 1.5 sprint, belong to the first-person body.
- `FirstPersonCameraController`: look only (mouse turns it every frame); the game places it at the first-person body's eyes.
- `MobaCameraController`: fixed-angle offset; follows an optional target, otherwise edge-pans while the cursor is confined.

Look controllers store yaw/pitch as `ViewAngles` (spherical coordinates, yaw 0 = -Z, positive yaw turns right, pitch clamped to 89 degrees) and pause while the mouse is not captured. `takeOver(camera)` continues from the camera's current direction.

## Control modes and the demo

Control modes (`ControlMode`, `gameplay/control_mode.h`): F1 first-person, F2 MOBA, F3 free camera (debug switching, the game constructor's `debugModeSwitching`).

### PlayerControls

They live in `PlayerControls` (`gameplay/player_controls`), which a game holds as a member (composition, so a game wanting other controls holds none).

- It holds the `Camera`, the three controllers, the mode, the MOBA lock and the first-person body.
- Built from a `PlayerControlsSetup` (free camera pose, MOBA look point, body's feet, field of view, near and far planes).
- Calls:
  - `spawnBody` (called from `onInit` where the game wants it among its entities);
  - `setControlMode`, `updateModeSwitching` (true when the mode changed);
  - `updateCamera(world, input, frameSeconds, alpha, mobaFollowTarget)`;
  - `updateFirstPersonCommands` (true while first-person input counts);
  - `poseFreeCamera`, `wantsMouseLook`, `getLightFocus` (the MOBA look point, else the camera);
  - and `visitSettings` (groups `firstPersonCamera`, `mobaCamera`, `freeCamera`).
- What a mode change means for the game's own things stays in the game: `DemoGame::dropMobaOrders` stops the duck, hides the marker and ends ground steering.

### First person

First person is a body (`gameplay/first_person_body`): `spawnFirstPersonBody` makes an entity with `Transform` (feet), `PreviousTransform`, `CharacterController` and `FirstPersonBody`.

`FirstPersonMoveSettings`:

| Setting | Value |
|---|---|
| walkSpeed | 5.5 |
| backwardMultiplier | 0.9 |
| backwardDiagonalMultiplier | 0.93333 |
| crouchSpeed | 3 |
| sprintMultiplier | 1.5 |
| standing height / eyes | 1.8 / 1.6 |
| crouching | 1.2 / 1.0 |
| eyeHeightSpeed | 6 |

The command is `wantsCrouch`; the state `crouching` and eased `eyeHeight`.

**Commands.** `PlayerControls::updateFirstPersonCommands` (from `DemoGame::updateFirstPersonCommands` in `onInput`):

- sets the controller's `wishVelocity` from `firstPersonWishVelocity(getMoveAxes, camera yaw, crouching, Shift held, settings)` (level forward turned by yaw, normalised, speed from the eight-direction table);
- `wantsJump` on a fresh Space press, `wantsCrouch` while Ctrl is held;
- and clears them outside first person or with the mouse released.
- While it counts, the demo throws a grenade on a fresh G press (`throwGrenade` from the camera along its forward, with `DemoGame::grenadeSettings` and `grenadeLooks`, a dark sphere).

**Moving the body:**

- `updateFirstPersonBodies(world, level, seconds)` crouches at once, stands up only if `characterFits` the standing capsule, eases the eyes and runs `moveCharacter`.
- The camera sits at `firstPersonEyePosition(world, body, alpha)` (interpolated feet plus eye height).
- F1 from the free camera calls `placeFirstPersonBody` (moves `Transform` and `PreviousTransform`, clears velocity, ground looked up afresh) with the feet an eye height below the camera. F1 from elsewhere looks straight ahead (-z) from the body.
- The body has no `Collider`, so picking never finds it.
- `characterFits(settings, feet, level)` (`physics/character_controller`) is true when the capsule overlaps nothing.

### MOBA mode

MOBA mode uses a confined visible cursor, edge panning (`gameplay/edge_pan`, from the cursor in `Input`), and Space to lock the camera on the player.

**Left-click to select** (`DemoGame::updateSelection`):

- a `CollisionScene` rebuilt and raycast in `Triangles` mode from the exact click position, through `rayThroughPixel`, shared with moving;
- nothing hit deselects; `getSelected`;
- `onDebugDraw` outlines the selection's collider in cyan even without F4;
- the demo's cubes and pillars share `cubeCollider`, the pyramid and teapot get their own mesh colliders, the moon and lamp bulbs unit spheres.

**Hovering** (`updateHover` in `onUpdate` after the camera moves):

- `pick(world, ray)`, shared with selection, raycasts the collision scene, then the static level up to that distance, and the nearer wins, so walls hide what is behind them;
- `getHovered` is the entity, outlined in dimmed cyan; `getHoveredTriangle` the level triangle, outlined with its normal only in the F4 view;
- with F4, readout lines 7 and 8 show colliders and level triangles, tested and cells walked, and line 9 the duck's controller (grounded or airborne, slope, substeps) with its ground normal drawn at its feet;
- the duck's controller has radius 0.5 and `mobaUnitAcceleration` (1000) so it starts and stops within a tick.

**The obstacle course** (`buildObstacleCourse`): cube parts of one entity at `obstacleCourseOrigin` (17, 0, -4) with the slab top at `obstacleCourseFloorTop`.

- A wall with a doorway, five 0.25-high steps to a platform, 20 and 50 degree ramps.
- Its collision mesh comes from the same parts, and it and the ground form `DemoGame::staticLevel`, `getLevel`.

**Right-click (or hold) to move the player** (`DemoGame::updatePlayerCommands`):

- the click ray hits the static level, so a step or platform top is the target;
- the marker sits `markerLift` above the hit;
- a click uses its exact event position;
- holding steers towards the live cursor via `groundSteeringActive`.

## Games and the light lab

### The game registry

The game registry (`app/game_registry`):

- `GameEntry {name, make}`: a plain function pointer making a fresh game, since golden images need one per view; the table is `constexpr`.
- `registeredGames()`: `lightlab`, then `demo`. Names are golden view names, since they name golden folders.
- `defaultGame()` (the first, the light lab), `findGame(name)`, `gameNames()`.
- `parseLaunchOptions(arguments)`: `--game <name>`, `--golden-save`, `--golden-check`, any order, each once; anything else throws `std::invalid_argument` listing the choices.

Both games start in first person. Adding a game is one line in the table.

### The light lab

`LightLabGame` (`games/lightlab/light_lab_game`, title "Light lab"): a sunlit test ground for shadows and DDGI.

- **A real sky:** an `Atmosphere` on its air entity while `LightLabSunSettings::atmosphere` is on, the default.
  - The sun is then its light above the air, 4, white, and the lab's own sky, ambient and disc are off.
  - The planet's ground is given the grass's albedo.
  - F5 Light lab, Atmosphere, for its numbers.
- **Controls:** the demo's `PlayerControls` (free camera from (0, 2, 6) looking north, MOBA look point (0, 0, -8), body at the origin, far plane 1000) and F4 for the engine's debug view.
- **The shared flashlight** (F, `gameplay/flashlight`, each switch a scene event over its reach): off at the start so the golden views keep the sunlit picture, held in the right hand in every mode and placed each frame after the camera, for watching a moving light's bounce in the probes.

**The back room's ceiling lamp** (L, `toggleLamp`, off at the start): an entity at `labLampPosition` (under the middle of the ceiling, 15 cm down).

- A plaster panel touches the ceiling (`labLampFixtureHalfSize`, casting no shadow, so it blocks neither its own light nor probe rays).
- While on: `labLampLight` (warm, intensity 2, range 7 m, not fast) and the panel glowing at `labLampGlow` (6) times its colour. Off removes the light.
- Each switch signals a light change round it (`probeEventAround(LightChange, labLampPosition, range)`).

**The clock:** `clockHoursSeen` follows the solar time as the ticks move it, so a gap in `onUpdate` (`signalClockJumps`, measured the short way round the day) is a hand on the slider. Past `labClockJumpHours` (0.1 h, small drags adding up) it signals a light change everywhere.

#### The level

Its level (`games/lightlab/light_lab_level`) is plain boxes, `LabBlock {centre, halfSize, LabSurface}` (Stone, Wood, Bark, Leaves, Plaster, Floorboards, RedPaint).

- `buildLightLabLayout()` builds them into four groups (`towers`, `fence`, `trees`, `building`).
- Each group is drawn as one entity of cube parts and, with the ground (`Mesh::plane(labGroundHalfSize)`, 400), added to the `StaticLevel`, so the triangles drawn are the triangles walked on.

What is in it:

- **Towers** at `labTowerDistances` (5, 10, ..., 320 m), each 0.1 times its distance wide and 0.4 tall, fanned 9 degrees apart from -27 so none hides another and all look the same size from the start.
- **A fence** of 0.1 m posts every 4 m from z = -4 to -300 at x = -6 with one 6 cm rail at 1 m, and a chain-link between them: one double-sided cut-out panel (`chainLinkPanel`, as tall as the posts) whose galvanised wire texture is made in code (`chainLinkImage`, 256 texels a 0.5 m tile, 8 diamonds a tile each way, 3 mm wire with soft edges, `chainLinkAlpha`), its mips built to keep the wire's coverage, so sun shadows, light probes and far mips all meet holes.
- **400 trees** (trunk and crown, sized 0.8 to 1.3) spread evenly over the ring 40 to 380 m out.
  - Placed by hashing each attempt's number (`Fnv1aHash` then `mixBits`, never a generator's running state).
  - Skipping the view wedge to the north (34 degrees either side plus 15 m) and 15 m round the building.
- **The building** south-west of the start, measured from `labSunlitRoom`, `labBackRoom`, `labWindow` (west wall), `labInnerDoorway` and `labFrontDoor` (east wall of the back room):
  - 0.3 m walls cut round the openings;
  - a 2 cm floorboard slab (`labFloorTop`, so it never fights the grass for depth);
  - a red panel lining the sunlit room's east wall;
  - a roof;
  - and a 1 cm pane of clear glass across the window (`labWindowPane`, `LabSurface::WindowGlass`: transmission 1, roughness 0.02, a faint green), in the middle of the wall's thickness, casting no shadow and kept out of the level's collision, so the sun still comes through.

#### The street

The street (`light-lab-street`; `LabSurface` Asphalt, Paving, Brick, Render, Metal, Glass): east to west along `labStreetZ` (45 m south of the start) from x = -150 to 150.

- **The road:** a 10 m road (2 cm asphalt slab) between 3 m pavements raised `labKerbHeight` (15 cm).
- **Houses:** rows of houses with their fronts on the pavements' edges (`labHouseFrontOffset`, 8 m from the centre line).
  - 10 m deep, 6 to 12 m wide, 3 or 4 storeys of 3 m, brick or render, each a solid block, cut by a 10 m cross street round x = 0.
  - Widths, storeys and surface hashed per house number (`StreetHashUse`, apart from the trees' uses), a house that would leave less than the narrowest taking the rest of its stretch.
- **Windows:** a window per 3 m bay and storey on each front (`LabWindow {pane, lit, tint}`: a 1.2 x 1.5 m pane straddling the front, standing 3 cm out; lit and tint hashed per window number, `labLitWindowShare` 0.4).
- **Lamps** (`streetLamps`, the bulbs' centres): 30 per kerb 10 m apart, the south side 5 m further east.
  - Each bulb 6 m up and 0.5 m in from the kerb.
  - A post set 0.6 m back towards the houses from the pavement to above the shade, an arm along the shade's top, and a 0.8 m shade just over the bulb, all Metal and static.
- The forest keeps `labStreetKeepClear` (10 m) from `labStreetBounds`.

**In the game:**

- The street's blocks and the windows (one part per window, in the layout's order, `getStreetWindows`) are static level.
- The bulbs' glow is one static renderer casting no shadow (visual only: it neither blocks the probes nor wakes them).
- Each lamp is an entity at its bulb holding, while on, `labStreetLampLight` (warm, intensity 12, range 15 m, sourceRadius 0.5, shadows, not fast).

**Switching the street lights.** `updateStreetLights` runs each frame after the daylight, and in `prepareGoldenView`:

- `streetLightsWantedFor(towardsSun)`: the sun below `labStreetLightsBelowDegrees`, -2.
- Flipped while `streetOverridden` (K toggles it; it clears whenever the sun's answer changes, at dusk or dawn).
- A change calls `setStreetLights`, which:
  - adds or removes every lamp's light;
  - sets the bulbs' glow (`labStreetBulbGlow`, 8, times the light's colour) and the lit windows' (`labWarmWindowGlow` to `labCoolWindowGlow` by tint, about 15 times the lamp-lit road);
  - and signals one light change over `labStreetBounds` grown by the lamps' range.

Golden view `street-night`: from (-40, 1.7, 43) down the road at 22:00.

### The sun

The sun (`scene/sun`). Local solar time; no equation of time or refraction.

- `solarDeclination(day)`: -23.44 degrees times the cosine of the year's phase from the December solstice, day 355.
- `solarHourAngle(hours)`: 15 degrees an hour from solar noon.
- `directionTowardsSun(latitude, declination, hourAngle)`, into the engine's +x east, +y up, -z north:

  east = -cos δ sin H
  north = sin δ cos φ - cos δ cos H sin φ
  up = sin δ sin φ + cos δ cos H cos φ

- `directionTowardsSunOn(latitudeDegrees, day, hours)` and `sunElevation`.

### The sky beyond the sun

The sky beyond the sun (`scene/celestial`, `scene/moon`, `scene/stars`; tested in `celestial_test` and `stars_test`).

**Time and frames:**

- `daysSinceJ2000(year, dayOfYear, hours)`: Gregorian leap days counted, the lab's solar time standing in for Universal Time.
- `eclipticToEquatorial(longitude, latitude)` and `equatorialDirection(rightAscension, declination)` (x the March equinox, z the celestial pole).
- `sunEclipticLongitude(days)`: the Astronomical Almanac's low-precision formula.
- `siderealAngle(days, solarHours)`: the sun's hour angle plus its right ascension, so the stars come round 3 min 56 s earlier each day.
- `equatorialToWorld(latitude, siderealAngle)`: a rotation with rows
  - east (-s, c, 0),
  - up (c cos φ, s cos φ, sin φ),
  - south (c sin φ, s sin φ, -cos φ);
  - a body goes where `directionTowardsSun(latitude, declination, siderealAngle - rightAscension)` puts it.

**The moon:**

- `moonEclipticPosition(days)`: Meeus chapter 47's mean elements and its largest terms, about 0.3 degrees; it puts the 2026 eclipses within 0.05 degrees of alignment.
- `moonPhaseAngle(towardsSun, towardsMoon)`: pi minus the angle between them.
- `moonPhaseBrightness(phase angle)`: Allen's curve, 0.026|a| + 4e-9 a^4 magnitudes (a quarter moon 9% of full).
- `moonAngularRadius` (0.00452).

**The catalogue:** `assets/stars/bright_stars.csv` (the Yale Bright Star Catalogue's 9,096 stars, trimmed, `assets/stars/README.md`).

- Read by `parseStarCatalogue`/`loadStarCatalogue` (`assets/star_catalogue`, an unknown B - V the sun's 0.65).
- `makeStar` turns each into a `Star`: equatorial direction, magnitude, colour `blackbodyColour(starTemperature(B - V))`.
  - Ballesteros's temperature, Planck's law against Wyman, Sloan and Shirley's fit of the CIE 1931 colour matching functions, into linear sRGB with a luminance of 1.

### The lab's night and day

The lab's night (`LightLabNightSettings`, group "Night"), all by eye, since the real moon is far darker than the eye adaptation makes up:

| Setting | Value | Meaning |
|---|---|---|
| moon intensity | 0.05 | at full above the air (chosen by the user by eye) |
| moon phase contrast | 0.25 | the phase curve raised to this power: 1 real, 0 every phase as bright as full |
| moon disc size | 5 | drawn five times its real size, for looks |
| star brightness | 1 millionth (`labStarBrightnessUnit`) | for magnitude 0 |
| star contrast | 0.7 | |

- Why the phase contrast: the real curve left a quarter moon 3.5 stops below full, all of it showing once the eye adaptation reaches its highest exposure, so 0.25 quarters the stops, as the eye's sense of brightness squeezes them. Chosen by the user by eye.

**At night**, `updateDaylight`:

- places the moon (`getMoon`, a `DirectionalLight` from `moonlightFor(towardsMoon, towardsSun, settings, atmosphere)`):
  - colour `labMoonColour` (1, 0.94, 0.86);
  - the full moon's intensity times the phase curve to the phase contrast's power;
  - `reflectsSun`, casting shadows, at the moon's angular size;
  - without the atmosphere fading below the horizon as the hand-made sun does;
- and turns the `StarrySky` (its stars loaded once in `onInit`, shared), its brightness times `starVisibility(towardsSun)`: none above -4 degrees of sun, all below -14. The dial makes stars thousands of times brighter than real, so they would otherwise show through the day sky.

`LightLabSunSettings::year` (2026) dates the moon. `advanceLabClock` carries the clock past midnight into the day and past the year's end into the year, so the moon moves on through the night.

**The day.** `LightLabSunSettings` (group "Sun"):

| Setting | Value | Meaning |
|---|---|---|
| latitude | 45 | |
| day | 80 | |
| solar hours | 15.5 | |
| hours per second | 0.025 | a day in 16 minutes; golden views hold it still |
| sun intensity | 3 | |
| sky brightness | 1 | the sky drawn and the flat ambient alike |
| sun disc size | 5 times the real one | for looks |

- Ticks advance the clock (`advanceLabClock`; `advanceSolarHours` wraps at 24 for the clock-jump check).
- Every frame `updateDaylight` sets the sun's `DirectionalLight`, the `AmbientLight` and the `SkyBackground` from `daylightFor(towardsSun, settings)`.
- It places the sun disc (`getSunDisc`: a black, emissive `Mesh::sphere` that casts no shadow and has no `PreviousTransform`) with `skyObjectPose` (`scene/sky_object`): at 0.9 of the camera's far plane along the way to the sun, scaled to `angularRadius × sunDiscScale` as the camera sees it, so it shows no parallax.
- The disc glows `discRadiance(colour × intensity, its angular radius)`:
  - π times the light value over the disc's solid angle `discSolidAngle` = 4π sin²(θ/2), since light values include Lambert's 1/π;
  - scaled down as a whole colour to `maxSkyRadiance`, 30,000, under the half-float limit, which full sun exceeds about 4.6 times;
  - hidden once the sunlight has faded to nothing.
- Sunlight fades in between -1 and 4 degrees of elevation and warms below 30 (from (1, 0.96, 0.9) to (1, 0.55, 0.3)).
- The sky's ambient and background (tint (0.6, 0.75, 1)) are full above 20 degrees and dim to a 3% night share at -6.
- Brightness values are by eye until lights have physical units.

## Characters

Characters: `spawnCharacter(world, assets, model, config, position)` creates an entity with:

- `Transform` (its position is the feet), `PreviousTransform`, `ModelRenderer`;
- `CharacterMovement` (a `MoveToController` plus turning state; `moveTo`/`stop`/`isMoving`);
- a fitted `Collider` capsule;
- and a `CharacterController` with `CharacterConfig::controller` (validated: positive radius, height at least two radii).

`updateCharacters(world, level, dt)`, for each character:

1. sets the controller's `wishVelocity` from `MoveToController::desiredVelocity(position, dt)` (across the ground at `speed`, cut on the last step to land exactly; zero with no target or time);
2. runs `moveCharacter`;
3. calls `updateArrival` (stops within `arrivalDistance`, 1e-4; a wall in the way means it never arrives: no path finding);
4. turns `CharacterMovement::yaw` towards the displacement at `turnSpeed`, then writes the Transform's rotation from it as a turn about +Y. The yaw is the character's own angle: set it, not the Transform, to face a character somewhere.

Helpers:

- `getCharacterVisualCentre(world, entity, alpha)` gives the interpolated camera target and `getCharacterFacing` the model's forward axis turned by the interpolated rotation.
- `math/ray` provides `Ray` and `intersectGround`; `scene/camera_ray` provides `makeCameraRay`. Together they turn mouse clicks into ground positions.

## Collision, particles and the character controller

### Shapes and overlaps

Collision (`physics/`): shapes live in one shared (world) space.

- `Sphere` (centre, radius), `Capsule` (segment `a`-`b` grown by `radius`; `a == b` is a sphere) and `Aabb` (`math/aabb`, also used for mesh bounds: `meshBounds`, `Model::getBounds`).
- `Shape` is a `std::variant` of the three.
- `overlaps(a, b)` exists for every pair in either order and for two `Shape`s (`std::visit`); touching counts as overlapping.
- Rounded shapes reduce to closest points between their cores, compared with the summed radii (`math/closest_points`):
  - `closestPointOnSegment`, `closestPointInBox`;
  - `closestPointsBetweenSegments` after Ericson;
  - and `closestPointsSegmentBox`, which cuts the segment where it crosses the box planes and minimises each quadratic piece exactly.

### Raycasts

Raycasts (`physics/raycast`): `raycast(ray, Sphere | Capsule | Aabb | Shape | TriangleMesh, maxDistance = unlimitedRayDistance)` returns `std::optional<RayHit>`:

- `distance` in world units (the direction is normalised inside);
- `point`;
- unit `normal` always against the ray.

Rules and methods:

- Starting inside a shape hits at distance 0 with the normal opposite the ray; a zero direction hits nothing.
- Spheres and capsule ends solve the ball quadratic, the capsule side drops the motion along its axis, boxes use slabs (the latest entry axis is the face), triangles use Moller-Trumbore from both sides and keep the nearest.
- `TriangleMesh` (`physics/triangle_mesh`: `positions` plus 3 `indices` per triangle, usually in world space) is built by `collisionMesh(mesh, transform)` (`scene/collision_mesh`, which checks indices).
- `Vec3::operator[]` reads x, y, z by index for per-axis loops.

### Colliders

Colliders (`physics/collider`): the `Collider` component is a `Shape` in the entity's own coordinates plus an optional `std::shared_ptr<const TriangleMesh> detail` (exact triangles, same coordinates, shared between entities; the shape must enclose them).

- `worldShape(shape, model)` moves a shape by a model matrix:
  - points exactly;
  - radii times the largest column length of the 3x3 part (exact for a `Transform`'s rotation times scale);
  - boxes to the axis-aligned box around the turned box (half-size `sum_j |A_ij| h_j`).
- `triangleMeshBounds`, and `meshCollider(mesh)` (box around the mesh plus the mesh).
- `raycastColliders(world, ray, RaycastDetail::Shapes | Triangles, maxDistance)` (`physics/collider_raycast`) builds a `CollisionScene` and asks it once (current poses, not interpolated).
  - The nearest `ColliderHit{entity, hit}` wins and shrinks the reach.
  - In `Triangles` mode a shape hit must be confirmed by `raycast(ray, mesh, model, maxDistance)`, which carries the ray into the mesh's coordinates with `affineInverse`, scales distances by world length per local length along the ray, and brings the normal back through `normalMatrix`.
  - `affineInverse` (`math/mat4`): `[A t; 0 1]^-1 = [A^-1, -A^-1 t; 0 1]`, `A^-1` from the cofactors `normalMatrix` also uses, nothing for a flat matrix.
  - A ray that slips between the triangles goes on to what is behind.
- `transformPoint` and `transformDirection` (`math/mat4`) apply a matrix with w = 1 or 0.
- `appendCollisionMesh` (`scene/collision_mesh`) adds a mesh to an existing `TriangleMesh` with shifted indices, and `AssetManager::makeCollisionMesh(renderer)` merges every part of a `ModelRenderer` (placed by its `localTransform`) into one.
- `spawnCharacter` gives characters a `Collider` capsule fitted to the drawn model (radius half its wider horizontal side, feet to head), with no detail.
- `DebugDraw::shape(shape, colour)` draws any `Shape`; `drawColliders(world, alpha, debug)` (`engine/debug_views`, interpolated pose, `colliderDebugColour` orange) runs in the F4 view.

### Broad phase

Broad phase: `UniformGrid` (`physics/uniform_grid`):

- cells of `CollisionSettings::gridCellSize`, default 2; `cellOf` uses floor;
- a `std::unordered_map` of occupied cells with the Teschner hash;
- an item goes in every cell its box touches, or on the large list past `largeItemCells`, 64;
- `clear` keeps memory and drops cells unused since the previous clear;
- mailboxing with per-item query numbers (`seenBy`, mutable, so not thread-safe);
- `query(region, found)` keeps items whose own box overlaps;
- `walkRay(ray, reach, visit)` (template, in the header) visits large items, clips the ray to the occupied cells' box, then steps cells near to far (Amanatides and Woo: `tMax`, `tDelta`) and stops when the next crossing is beyond the reach `visit(item, reach)` returns, returning `GridRayWalk{cells, items}`.

`CollisionScene` (`physics/collision_scene`) owns `CollisionSettings`, read at each `rebuild(world)`, which clears and refills from every `Collider` + `Transform` (world shape, model matrix, detail). It answers:

- `raycast(scene, ray, detail, maxDistance, CollisionQueryStats*)` (narrow phase per visited collider; a triangle miss leaves the reach);
- and `overlapping(scene, shape)` (shapes only).
- Rebuilding rather than updating is deliberate: the ECS has no change tracking, so incremental updates could leave ghost entries.
- `boundsOf(Shape)` (`physics/shapes`).

### Readout lines and profiling

`debugReadoutLine(n)` and `debugReadoutTextHeight` (`engine/debug_views`) place readout lines. The engine uses 0-9, and a game starts at `firstGameReadoutLine` (10):

| Line | Constant | Shows |
|---|---|---|
| 4 | `probeReadoutLine` | the light probes |
| 5 | `videoMemoryReadoutLine` | the video memory |
| 6 | `exposureReadoutLine` | the eye's exposure |
| 7 | `cameraReadoutLine` | the camera's position and view direction (`cameraText`, "CAMERA x y z  LOOK x y z", for setting up a golden view where a problem was seen) |
| 8 | `engineProfileReadoutLine` | its CPU timings |
| 9 | `gpuProfileReadoutLine` | its GPU timings per pass |

Profiling: `Profiler` (`core/profiler`, plain C++ so games and tests stay headless) has numbered sections (`addSection` once, `record(section, seconds)`).

- It counts per call and publishes each section's average, worst call and call count (`SectionTiming`) when `update(now)` (once a frame) finds a window (`defaultProfileWindowSeconds`, 0.5) has passed, zeros for a section that did not run.
- `profilerSeconds()` is `steady_clock` in seconds; `ScopedTimer` records its block and `timeSection(profiler, section, work)` times a callable.
- `Application` owns one (FRAME, WORK = frame minus the cap's sleep, TICK per tick, UPDATE, SCENE = building the frame and debug lines, RENDER = `renderer.render`, which includes waiting for the GPU) and draws it with `drawProfile` (`profileText`: name, average/worst ms).
- `DemoGame` times each system in `onFixedUpdate` into its own `systemTimes`, advanced in `onUpdate` and drawn on the first game line (averages, three decimals).

### Static level

Static level (`physics/static_level`): `StaticLevel` holds everything that never moves as one world-space `TriangleMesh` plus its own `UniformGrid` over triangle numbers.

- Built as `add(piece, model)` is called (validated first, so a bad piece changes nothing) and never rebuilt; cell size from the `CollisionSettings` given at construction.
- `raycast(level, ray, maxDistance, stats)` walks the grid with `raycastTriangle` and returns `LevelHit{hit, triangle}`.
- `contacts(level, capsule)` queries the grid with `boundsOf` and returns a `Contact{point, normal, depth, triangle}` (`physics/triangle_contact`) per touched triangle (touching counts, depth 0).

**Triangle contacts.** `contact(capsule or sphere, a, b, c)`:

- nearest points between core and triangle (`closestPointsSegmentTriangle`: a piercing crossing, else the best of both ends against the triangle and the segment against each edge; `closestPointOnTriangle` by Voronoi region, Ericson 5.1.5, nearest edge for a flat triangle);
- normal from triangle to core and depth radius minus distance;
- a core touching the triangle uses the face normal on its middle's side and depth = deepest end behind the plane + radius.
- Triangles are two-sided.
- `raycastTriangle` (`physics/raycast`) is the one Moller-Trumbore test, shared with mesh raycasts. Its barycentric checks allow `edgeTolerance` (1e-5) outside 0..1 so rays through a seam shared by two triangles cannot slip between them (a demo bug, kept as a bit-exact regression test).
- `CollisionQueryStats{tested, cellsWalked}` lives in `physics/query_stats`.

### Sphere sweeps

Sphere sweeps: `sweepSphere(sphere, motion, a, b, c)` (`physics/raycast`) is the first touch of a moving sphere against one triangle.

- The centre's path is raycast against the triangle grown by the radius (two faces offset along the normal plus a `Capsule` per edge).
- A sphere already touching hits at distance 0 with the contact normal if moving into it (more than `glancing`, 1e-5) and ignores it otherwise.
- The hit's `point` is the centre at the touch, its normal from the triangle to the centre.
- `sweepSphere(level, sphere, motion)` (`physics/static_level`) queries the grid with the box round the start and end spheres and returns a `LevelHit`.
- Not built: box against triangle (separating axes), capsule and box sweeps.

### Particles

Particles (`physics/particle`): `ParticleSettings`:

| Setting | Value |
|---|---|
| radius | 0.1 |
| gravity | 9.81 |
| restitution | 0.45 |
| friction | 0.4 |
| restingSpeed | 1 |
| maxBounces | 4 |

- The `Particle` component: settings, `velocity`; the `Transform` position is the centre.
- `fly(velocity, acceleration, seconds)` returns a `FlightStep` moved by the average of the start and end velocities, exact for constant acceleration.
- `bounceVelocity(velocity, normal, settings)`:
  - the normal part comes back times restitution (0 below restingSpeed);
  - friction slows the sliding part by at most friction (1 + e) times the speed in, never reversing it (resting contact therefore decelerates at mu g).
- `moveParticle(particle, centre, level, seconds)`:
  1. push out of overlaps (deepest first, ending `clearance` 1e-3 clear);
  2. if a surface within 2 clearance is moved into, gravity is added at once and the surfaces push back (`pressAgainstTouching`) and the ball moves at the result; otherwise it flies under gravity;
  3. then sweeps, stopping `clearance` off each surface hit, bouncing with the velocity at the moment of the hit (share of the path stands in for share of the time) and flying on, up to maxBounces.
- A resting ball stays exactly still.
- `moveParticles(world, level, seconds)` for every `Particle` + `Transform`.

### Grenades

Grenades (`gameplay/grenade`):

- `GrenadeSettings` (throwSpeed 15, lifetimeSeconds 5, `particle`);
- the `Grenade` component (`secondsLeft`);
- `throwGrenade(world, from, direction, looks, settings)`: Transform scaled to the radius, PreviousTransform, the looks, a unit `Sphere` `Collider`, a `Particle` with velocity along the normalised direction, the thrower's velocity not added;
- `updateGrenades(world, seconds)` destroys expired ones inside its `each` (deferred by the `World`).

`Contact::faceNormal` is the triangle's own normal turned to the shape's side (equal to `normal` on the face, different on an edge or corner).

### Character controller

Character controller (`physics/character_controller`): `CharacterControllerSettings`:

| Setting | Value | Note |
|---|---|---|
| radius | 0.4 | |
| height | 1.8 | |
| maxSlopeDegrees | 45 | |
| stepHeight | 0.3 | |
| snapDistance | 0.3 | |
| gravity | 17.5 | Overwatch's community-measured values |
| maxFallSpeed | 30 | Overwatch's community-measured values |
| jumpSpeed | 5.72 | Overwatch's community-measured values |
| groundAcceleration | 100 | guesses to tune by feel |
| airAcceleration | 30 | guesses to tune by feel |
| substepFraction | 0.5 | |
| pushOutPasses | 8 | |

The `CharacterController` component holds:

- the settings;
- the commands `wishVelocity` and `wantsJump` (used up by the next tick, honoured only on the ground);
- and the state `velocity`, `grounded`, `groundNormal`, `lastSubsteps`, `groundKnown` (a new controller probes the ground on its first tick).
- `controllerCapsule(settings, feet)`.

`moveCharacter(controller, feet, level, seconds)`:

1. Jump first (so take-off uses air rules).
2. `steer`:
   - ground: `moveTowards` the wish at groundAcceleration;
   - air: no wish keeps momentum, steering at airAcceleration keeps the speed, up to the wished speed, for turns of 90 degrees or less.
3. Vertical velocity by semi-implicit Euler with quadratic drag while falling, with v_t = maxFallSpeed:

   `a = -g + (g / v_t^2) v^2`

4. Grounded movement is the horizontal velocity tilted into the ground plane and rescaled to full speed (slopes walked at the same speed).
5. Substeps of at most radius * substepFraction (64 at most), each followed by `pushOut`: deepest contact first, re-queried each pass.
   - Contacts that `supports` the character push straight up by depth / normal.y and stop falling.
   - Others push along the normal and remove the velocity into it, steep upward normals only sideways while walking.
6. Then, if it was grounded and did not jump, `descend` settles it onto the ground up to snapDistance in `descentIncrement` (0.05) drops, pushing out at the first overlap, which also removes hovering. Dropping the whole way at once let a kerb's edge win the push-out and shove it sideways.
7. `groundUnder` (contacts of the capsule lowered `groundProbeDistance`, 0.01, that support it) decides `grounded` and `groundNormal`.

`supports`: a walkable face (normal.y >= cos maxSlope), or an edge or corner contact (normal differs from `faceNormal`) facing up at least `leastStepNormalY` no higher than stepHeight above the feet, so the capsule rides up low ledges (no separate step-up move).

`moveCharacters(world, level, seconds)` runs it for every `CharacterController` + `Transform`. `degreesFromRadians` joins `math/angles`.

## Meshes and colour

### Meshes

Meshes: `Mesh` (`scene/`) is the import format: `Vec4` positions plus triangles with an optional normal, UV and tangent per corner, and `uvOrigin` (`UvOrigin::BottomLeft` for OBJ and code, `TopLeft` for glTF). Loaders, bounds and the software renderer use it.

**The render format.** `buildIndexedMesh` (`scene/indexed_mesh`) turns it into the render format, `IndexedMesh`: unique `MeshVertex` (position, normal, UV, tangent: 48 bytes, the exact GPU vertex layout, read with `offsetof`) plus 32-bit indices.

- `resolveCorners` gives each corner its file normal or its face normal, its UV or (0,0), and its tangent.
- `buildIndexedMesh` then merges corners identical in every component (the key normalises -0.0 to +0.0).
- The engine builds the `IndexedMesh` once when a mesh is first uploaded; `VulkanMesh` only validates (`checkMeshUpload`) and uploads it.

**Normals.** Files without normals stay flat shaded, so their corners rarely merge, unless loaded with `smoothNormals`.

- `generateSmoothNormals(mesh, creaseAngle)` (`scene/smooth_normals`) fills in missing corner normals by averaging the faces around each position (grouped by exact position, so seams with duplicated vertices smooth too), weighted by corner angle, counting only faces within the crease angle (default 60 degrees) of the corner's own face.
- It is opt-in because only the game knows whether a shape is meant to be faceted.
- `faceNormal` lives in `scene/mesh`.

**Tangents** (`MeshVertex::tangent`: xyz the unit direction of increasing u, w = ±1 with the bitangent "up the picture" = w · (N × T)):

- They come from the file when every corner has one (glTF `TANGENT`, normalised, w must be ±1).
- Else from `generateTangents(corners, origin)` (`scene/tangents`): vendored MikkTSpace (`external/mikktspace`, compiled as C99 by its own makefile rule) on the unshared corners, given (u, 1 − v) for `TopLeft` meshes so the bitangent points up the picture as Blender's exporter makes it.
  - `tangents_test` regenerates NormalTangentMirrorTest's exported tangents and matches at least 99% of corners, signs included.
- A mesh whose UVs are all equal gets any tangent across each normal with sign +1, and MikkTSpace's placeholder for UV-less patches is put across the normal by one Gram-Schmidt step.
- Mirror seams (same position, normal and UV, opposite sign) stay separate vertices.

### Colour and textures

Colour is gamma-correct: shading happens in linear light.

- `VulkanTexture` takes a `VulkanTextureDescription`: format, bytes per texel, and `surface` (a full mip chain, repeat and anisotropy, or else a one-level clamped lookup table).
- `surfaceTexture(encoding)` gives `R8G8B8A8_SRGB` for colour (the GPU decodes texels when sampling and when averaging mips) or plain `R8G8B8A8_UNORM` for data (metallic-roughness and normal maps).
- `VulkanRenderer::uploadTexture` takes the `TextureEncoding`, and `textureOrWhite` picks a material's texture or the white stand-in (white multiplies colours and factors by 1).
- The SDR swapchain has an sRGB format that encodes on write (`B8G8R8A8_SRGB`, see VulkanRenderer).
- `Material::colour` bytes are sRGB like image files; `packMaterial` decodes them with `srgbByteToLinear` (`core/srgb`).
- glTF colour factors are linear, so the loader encodes them with `linearToSrgbByte`.
- Light colours, intensities and `emissive` are linear.
- `VulkanTexture` uploads RGBA pixels with a full mip chain (`mipLevelCount`, `render/gpu/mip_levels`; each smaller level blitted from the one above, or copied in when built on the CPU: a cut-out's) and samples them with trilinear and 16x anisotropic filtering. The sampler's `maxLod` is `VK_LOD_CLAMP_NONE`: 0 would lock sampling to the full-size level.
- `Material::emissive` (a `Vec3`, black by default) is light the surface gives off itself, added after lighting. With a black `colour` the surface shows only its glow and never changes with lighting (the demo's moon and MOBA marker).
- `GpuMaterial` is 96 bytes.
- `Mesh::sphere(segments, rings)` builds a unit UV sphere with smooth normals.

## Loaders

Loaders:

- `obj_loader`: quads/convex polygons, normals, UVs.
- `gltf_loader` (cgltf): returns a `Model` of `ModelPart`s with world transforms, the material (see Assets and materials) with its textures external or embedded in `.glb`, and per-corner normals, UVs and tangents.
- Occlusion and emissive textures are ignored for now. glTF texture transforms are not supported.

## Software renderer

Software renderer (reference path, used only by tests): `Renderer`, `Display`, `PixelBuffer`, `DepthBuffer`, `rasterizer`, `clipper`, `shading`. It does flat shading only and ignores per-vertex normals and UVs.

## Light, HDR, tone mapping, bloom and shading

### Light components

lights are components (`scene/light.h`):

- `DirectionalLight`: stores the direction the light travels; no `Transform` needed.
- `PointLight`: position from the entity's `Transform`, fades to exactly zero at `range`, `sourceRadius` like a spotlight's, packed into `radiance.w`.
- `SpotLight`: a point light in a cone.
  - It stores its `direction`, full brightness inside `innerAngle`, smoothly fading to zero at `outerAngle`.
  - A `sourceRadius` softens the falloff up close: `intensity / (d² + r²)`.
  - The shader compares cosines, which `packLighting` precomputes and clamps inner to outer.
- `AmbientLight` (several add up).
- `SkyBackground` lights nothing: its colour (linear, several add up, black without one) becomes `FrameLighting::background`, the colour the main pass clears the scene image to, so it shows wherever nothing is drawn; with an `Atmosphere` in the frame, the sky is drawn over it.
- Colours are 0..1 tints with a separate unbounded `intensity`.

**Each frame:**

- `buildFrame` calls `collectLighting(world, alpha)` (`scene/lighting`, point light positions interpolated like meshes).
- `VulkanRenderer::drawScene` (after `prioritizeLights`) uses `packLighting` (`render/gpu/light_uniforms`) to fill `LightUniformData` and pushes it once to fragment uniform slot 1.
- That struct mirrors the `LightData` block in `triangle.frag` byte for byte (all `vec4`, `static_assert` 2720 bytes).
- Light positions are measured from the camera (`packLighting(lighting, cameraPosition)` subtracts it), so the viewer sits at the shader's origin and has no field.

### HDR and tone mapping

HDR and tone mapping: `triangle.frag` writes unbounded linear colour into `sceneColour`, an `R16G16B16A16_SFLOAT` texture the size of the frame (made with the depth texture in `ensureFrameTextures`).

A final pass (`beginToneMapPass`, `fullscreen_triangle.vert` + `tone_map.frag`, `ToneMapUniformData` at fragment slot 0, the scene read with `texelFetch`):

1. multiplies by `ToneMappingSettings::exposure` (1.5, so scenes tuned for the old clipped look keep their shadows and mid-tones);
2. applies the curve (`ToneMapCurve`), stretched to the screen's peak, `sdrPeakBrightness` 1 on an SDR screen:

   `peak * curve(x / peak)`

   - `Clamp`, the pre-HDR look;
   - `Reinhard`;
   - `Aces`, Narkowicz's fit;
   - `AgX`, the default, Wrensch's minimal form;
3. and writes the swapchain, times the white level.

**HDR output.** Before acquiring each frame, `updateDisplayOutput` reads the window's `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, headroom and SDR white level (`readWindowDisplay`; whether an `EXTENDED_SRGB_LINEAR` swapchain is supported is asked only when HDR comes on). `chooseDisplayOutput` then picks `DisplayOutput{hdr, peakBrightness, whiteLevel}`:

- HDR when `DisplaySettings::hdrOutput` and the screen allow it;
- white at `paperWhiteNits / 80` (scRGB's 1 is `scrgbWhiteNits`, 80) or Windows' level;
- the peak at `peakNits` or the monitor's (headroom times its white), never past the monitor's nor below white, in units of white.

When `hdr` changes `prepareSwapchain` waits for the GPU, rebuilds the swapchain in the other format and colour space (keeping the present mode; a refused HDR swapchain is logged and marked unsupported) and rebuilds the swapchain-format pipelines (`createScreenPipelines`: tone mapping, debug lines and the overlay), logging the peak and white.

- `ToneMapUniformData` (48 bytes, with night vision's vec4) carries the white level and whether to dither (only on SDR), plus the bloom strength (`parameters.w`, 0 = none) and scale (`output.z`).
- Debug line colours are multiplied by the white level (`buildDebugLineVertices(lines, colourScale)`).
- `getDisplayOutput()`.

### Bloom

Bloom (`render/gpu/bloom`, `BloomSettings`):

| Setting | Default |
|---|---|
| `enabled` | |
| `strength` | 0.04 |
| `radius` | 1 |
| `fireflyReduction` | |
| `maxLevels` | 6 of at most `BLOOM_LEVEL_LIMIT` 8 |

Between the main pass and tone mapping, `VulkanBloom` (`render/vulkan/vulkan_bloom`, owned by `VulkanRenderer`):

1. halves the scene down a chain of separate `B10G11R11_UFLOAT_PACK32` images (16-bit float if unsupported; separate textures, not mips, so a pass never reads what it draws into):
   - level 0 half the frame;
   - `bloomLevelCount` stopping before a side drops below `BLOOM_SMALLEST_LEVEL_SIDE` (4);
   - with `bloom_downsample.frag` (Jimenez's 13-tap filter as five weighted 4x4 boxes; the Karis average, boxes weighted by 1 / (1 + luminance) and renormalised, on the first halving only);
2. then grows it back with `bloom_upsample.frag` (3x3 tent, reads `radius` texels apart, additive blending onto the level's own halving).

`tone_map.frag` reads level 0 with a linear sampler (fragment sampler slot 1; the scene stands in when there is no bloom) and mixes before exposure, so light is moved, never added:

  `mix(scene, bloom / levelCount, strength)`

`bloomBlur`, `bloomDownsample`, `bloomUpsampleAdd`, `sampleBilinear` and `mixBloom` mirror the shaders on the CPU for tests (energy kept, flat images unchanged, mirror symmetry).

### Dithering

Its last step dithers: it encodes the colour to sRGB, adds up to half an 8-bit step of interleaved gradient noise (from `gl_FragCoord`), and decodes, so dark gradients do not band.

- `core/dither` mirrors and tests it.
- The GLSL lives in `assets/shaders/dither.glsl`, a shader-only include with the sRGB curve and the noise, which `triangle.frag` also uses for soft shadows; `tools/package_demo.py` skips `.glsl` files.

### Physically based shading

Shading is physically based (glTF metallic-roughness, `render/gpu/brdf`, mirrored and tested on the CPU like the other shader maths). Its numbers, dielectric reflectance 0.04, smallest roughness 0.045, smallest view cosine and the table size 32, are in the macro header `render/gpu/brdf_shared.h`.

**Per light:** the GGX distribution times Smith height-correlated visibility times Schlick Fresnel, scaled by a multiple-scattering energy compensation (Turquin 2019)

  `1 + f0 (1 / E - 1)`

plus Lambert diffuse weighted by `1 - specular albedo`, so a white surface under uniform light returns exactly what it receives (the white furnace test).

**Unit convention:** the BRDF is multiplied by pi, so a light's radiance is the irradiance it gives a surface facing it and a white matte surface lit head-on by 1 shows 1 (the pre-PBR brightness).

**`ShadingPoint`** (shader and CPU, `prepareShading`) is worked out once per pixel:

- `f0 = mix(0.04, baseColour, metallic)`, alpha = roughness squared;
- and from the specular albedo table, the compensation, the specular albedo and the weighted diffuse colour.
- The table: `BrdfTable`, scale and bias for f0 over n.v and roughness, texel i at i / (size - 1).
  - Integrated at start-up by `buildBrdfTable` with GGX importance sampling along a Hammersley sequence, 4096 samples per texel, about 85 ms in release.
  - Uploaded by `VulkanRenderer::createFixedTextures` as `R16G16_UNORM` via `brdfTableTexels`.

Ambient returns `ambient * (diffuse + specularAlbedo)`, so metals are not black under ambient light.

Fragment samplers: 0 colour, 1 shadow atlas (compared), 2 shadow atlas (raw), 3 metallic-roughness, 4 BRDF table, 5 normal map.

**Normal mapping** (mirrored in `render/gpu/normal_mapping`: `decodeNormalTexel`, `perturbNormal`, `transformTangent`, `toksvigRoughness`):

- `triangle.vert` carries the tangent with the model matrix, normalises it like the normal and multiplies its sign by `handedness(model)` (`math/mat4`, pushed per draw after the three matrices, 52 floats).
- `triangle.frag` decodes the texel as (2c − 1) · (scale, scale, 1), rebuilds the bitangent as w · (N × T) from the interpolated, unnormalised normal and tangent (MikkTSpace's rule) and normalises only the sum.
- Shadow offsets use the geometric normal.
- Normal map filtering (Toksvig 2005, `RenderSettings::normalMapFiltering`, on by default, sent as `textureOptions.y`): the mips are averaged and never renormalised, so a mip texel's length L (decoded without the scale) tells how much the bumps under it disagree. L is first lengthened by `NORMAL_TOKSVIG_QUANTIZATION_SLACK` (sqrt(3)/255, `normal_mapping_shared.h`: what 8-bit rounding alone can take off) and capped at 1; the spread (1 − L) / L, times the scale squared, adds twice over to alpha² (alpha = roughness²), at most roughness 1. A full-size texel gives the material's roughness back exactly. The normal map is read once, before the roughness, outside any branch.

**Odds and ends:**

- The demo's glowing materials (moon, bulbs, floodlight heads, marker) share `glowMaterial` (black, roughness 1); the teapot is real gold (metallic 1, roughness 0.35).
- `Vec3 * Vec3` multiplies component by component, as GLSL does.
- Limits are 4 directional, 64 point and 8 spotlights (2736 bytes); with more, `prioritizeLights` picks the most important (see Shadows above).
- The software renderer still hardcodes a light at `(-1, 2, 1)`.

### The demo's lighting

- The demo's ambient 0.2 plus sun 0.8 reproduces the old hardcoded look.
- It is night (ambient 0.012 (`nightAmbient`), tiny because linear values are brightened a lot by the sRGB encoding).

**The moon:**

- A glowing moon sphere sits at a fixed place 64 units from the free camera's start (about 20 seconds of flying in F3 mode, so it can be reached; it therefore shows parallax, unlike the real moon), in the direction the moonlight comes from.
- The moonlight is a bluish shadow-casting `DirectionalLight` at intensity 0.06 (`moonlightIntensity`).
  - It was a `PointLight` inside the sphere for a while, to try point light shadows; directional shadows are much sharper because their box covers only 30 units.
- The sphere's `ModelRenderer` has `castsShadows = false` so it does not block the light it stands for.

**The flashlight.** F toggles a flashlight (`gameplay/flashlight`: `Flashlight`, a `SpotLight` entity removed when off, the beam `flashlightBeam`, shared with the light lab) that `DemoGame::updateFlashlight` places every frame in `onUpdate`:

- in the right hand (`flashlightInHand`: 0.4 right of and 0.3 below the eye, along the camera's up, aimed at the point 8 units straight ahead, so its shadows show beside things);
- or in MOBA mode held by the duck using its interpolated pose and `getCharacterFacing` (`gameplay/character`), tilted down 20 degrees and held 1.2 units in front of it, past its body, so the duck does not block its own light.

**The lamp field.** L toggles a lamp field (`DemoGame::toggleLamps`): 100 coloured point lights, more than the 64 point light shadows, so light priority has to choose.

- Emissive bulb spheres with `castsShadows = false`.
- A 10 x 11 grid 3.5 apart minus 10 places in a keep-clear box around the spinning objects, the duck and the first-person start.
- Each with a stone pillar beside it turned a golden angle further each time.
- Each lamp is a `Firefly` (`gameplay/firefly`: `fireflyPosition(firefly, seconds)`, a circle round the pillar whose radius, height and speed wobble as sums of sines, staying 0.55-0.95 from the pillar and below its top) with a `PreviousTransform`, so the pillar shadows sweep round and every lit lamp's shadow tiles are redrawn each frame (a shadow stress test).
- The entities are destroyed when toggled off.
- The lamps are `fast` point lights: they fly, and the still memory of 2 s would leave their bounce trailing. Measured, debug build, RTX 5080: the lamp field view's probe pass 1.65 ms instead of 1.38 with them still.
- Each switch is one `GeometryChange` scene event (the pillars come and go) round every lamp and pillar, reaching `lampRange` plus the orbit and its wobble (`signalLampField`, through the file's `signalAround`, which also signals each floodlight switch as one `LightChange` over the four beams' `floodlightRange`).

**Light probes.** The demo has light probes too: one `ProbeCascades` round the camera, the ground and the obstacle course static.

- Its night's rays that miss bring back the flat ambient (0.012), so out of the lights the ground and course now shade it from below and beside (darker than the flat ambient alone).
- The lamps', floodlights' and flashlight's bounce is most of what the probes add.

**The floodlights.** K toggles four floodlights over the obstacle course (`DemoGame::toggleFloodlights`, on at the start, `getFloodlights`):

- shadow-casting cool-white `SpotLight`s (intensity 40, range 25, cone 20 to 35 degrees, `emitterRadius` 0.15 for soft shadows);
- whose glowing cube heads (`castsShadows = false`) hang 0.5 out from poles at `floodlightPoleFeet()` (just inside the slab's corners, `floodlightHeight` 8 up);
- each aimed 0.3 of the way past the course's middle so the beams cross.
- The poles are parts of the course renderer, so they are in the static level and stay when the lamps are off.
- All tuning is in named `floodlight...` constants in `demo_game.cpp` (`floodlightCount` and `floodlightHeight` in `demo_game.h`, which `floodlightPoleFeet` also needs).

### Eye adaptation

Eye adaptation (`eye-adaptation`; `render/gpu/eye_adaptation`, numbers in `eye_adaptation_shared.h`, mirrored by `assets/shaders/eye_adaptation.glsl`, tested in `eye_adaptation_test` and `eye_adaptation_gpu_test`; `render/vulkan/vulkan_eye_adaptation`): an exposure that follows the scene's brightness, as an eye adapts between noon and moonlight.

It runs after the main pass (which leaves the scene image `SurfaceRead`, readable by compute too) and before bloom, as GpuPass `EyeAdaptation` (EYE), in two passes.

**1. The histogram.** `eye_histogram.comp` sorts every pixel's luminance (`pixelLuminance`, Rec. 709 as bloom) into 256 bins a tenth of a stop wide from -16 to +9.6 EV (`histogramBin`; nothing and darker in the first, brighter in the last).

- Each 16 x 16 workgroup counts in shared memory and adds its non-empty bins to the frame's histogram (zeroed by a fill) with atomics, so the result is the same in any order.

**2. The adaptation.** `eye_adaptation.comp` (one invocation):

- takes the mean in stops of the pixels between the darkest 50% and the brightest 10% (`histogramMeanEv`: each bin weighted by its part inside; the black sky and the moon, sun and bulbs left out);
- works out the target exposure within `lowestExposureEv`..`highestExposureEv` (`targetExposureEv`):

  `adaptationStrength` x (log2 `adaptationReference` - mean)

- and steps towards it, e^(-dt / tau) of the gap left, tau `darkAdaptationSeconds` when the exposure rises (the scene got darker) and `brightAdaptationSeconds` when it falls (`adaptExposureEv`).
- Into the dark only after the scene has stayed darker for `darkAdaptationDelaySeconds` (1 s), counting only the time past it, a brighter frame ending the wait (`holdThenAdaptExposure`: a light going out reads as going out, and a flickering one never sets the eye off).

**The result** goes into a 32-byte buffer kept in GPU memory from frame to frame (`GpuEyeAdaptation`):

- exposure, target, mean, measured, the wait;
- and the adapted mean with its own wait: the mean stepped by `holdThenAdaptExposure` in negated stops, so it waits into the dark and follows into the light as the exposure does, snapped with it; night vision's eye.

`tone_map.frag` reads it the same frame (set 2 binding 2; `ToneMapUniformData::output[3]` 1 while it adapts), multiplying `ToneMappingSettings::exposure`, now a bias, by 2^exposure.

- No CPU round trip: a copy per frame in flight goes to read-back memory only for the F4 readout (`exposureText`, its own line, `exposureReadoutLine` 6).
- It snaps to its target (`GpuEyeAdaptationStep::snap`) on its first frame, when switched back on, and on every frame a capture draws, so golden images do not depend on earlier frames.

**The settings** (`ToneMappingSettings`, the Tone mapping group):

| Setting | Value | Why |
|---|---|---|
| eye adaptation | on | |
| reference | 0.3 | the light lab's daylight towers measure -1.7 EV, so they keep their look |
| strength | 0.5 | a night five stops darker shows 2.5 darker |
| dark | 1 s after a 1 s delay | |
| bright | 0.7 s | |
| range | -4 to +1 EV | the user chose by eye to brighten dark scenes by at most a stop, so a night stays a night |

With the bias, `exposure`, now 1 rather than 1.5.

**Measured means:**

| View | Mean |
|---|---|
| the lab's daylight views | -1.2 to -1.9 EV |
| the sunlit room | -6.1 |
| the moonlit night | -6.6 |
| the demo's night | -5.3 to -7.4 |
| its moon view | -16 |

### Night vision

Night vision (`moon-phase-contrast`; `rodLuminance`, `rodShare`, `nightVision` in `render/gpu/tone_mapping`, numbers in `tone_mapping_shared.h`, mirrored by `assets/shaders/tone_mapping.glsl`, tested in `tone_mapping_test` and `tone_mapping_gpu_test`): `tone_map.frag`, after bloom's mix and before the exposure, blends each pixel towards the rods' grey by the rods' share.

**The rods' grey:** `rodLuminance` (Ward Larson et al. 1997), from linear sRGB's CIE XYZ (`SRGB_TO_X_*` and `SRGB_TO_Z_*`, Y `pixelLuminance`):

  Y (1.33 (1 + (Y + Z) / X) - 1.68)

- Divided by white's, so white keeps its brightness and blue looks brighter, red darker (the Purkinje shift).
- X is 0 only for black, and the bracket is positive for every colour.
- Times a tint: white at no blue shift, `NIGHT_TINT` (0.75, 0.9, 1.35) scaled to luminance 1 at full.

**The rods' share:**

  1 - smoothstep(`rodsFullEv`, `conesFullEv`, max(the pixel's stops, the eye's))

- The eye's is the adapted mean (none without eye adaptation: the pixel alone), so a pixel goes grey only while both are dim; a lamp in a dark street and a dark corner of a sunlit room keep their colour.
- Written colour + (grey - colour) share, so a share of 0 leaves the colour exactly.

**The settings** (`ToneMappingSettings`):

| Setting | Value |
|---|---|
| night vision | off by default for now (the user's choice, until physical light units) |
| rods fully | below -9.5 EV |
| cones fully | above -7 |
| blue shift | 0.5 |

- Chosen by measuring the light lab's golden scene images (means: the moonlit field -8.2, the sunlit room by day -6.2, the street at night -4.1), since the engine's light has no physical units (the real thresholds are about 0.005 and 3 cd/m2) and the lab's moon is about 6,700 times too bright against its sun.
- `ToneMapUniformData::nightVision`: rods, cones, blue shift, on (the block is 48 bytes).
