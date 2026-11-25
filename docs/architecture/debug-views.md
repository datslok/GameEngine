# Debug drawing and the F4 readout

Covers: `src/scene/debug_draw`, `src/scene/debug_text`, `src/engine/debug_views`, `src/render/render_stats.h`, `src/render/gpu/video_memory`, `src/render/gpu/debug_line_vertices`, `src/render/vulkan/vulkan_debug_lines`, `assets/shaders/debug_line.frag`, `assets/shaders/debug_line.vert`

What games and systems draw to look inside the engine, and the frame's numbers on screen. The frame itself: `game-interface.md`.

## Debug drawing

`DebugDraw` (`scene/debug_draw`): immediate-mode lines, cleared every frame. Colours linear.

- World lines: `line`, `box(bounds, transform)`, `sphere` (three circles), `capsule`, `shape` (any collision `Shape`).
- `screenLine`: window pixels, top-left origin.
- Readout layout: `takeReadoutLine` hands out the next text line (engine's first, then the game's); `setScreenWidth` (the last frame's `outputWidth`) for wrapping.
- `debugText`/`debugTextWidth` (`scene/debug_text`): stroke font on a 4 x 6 grid, A-Z (lower case as upper), 0-9, space, `: - / . ?`; anything else as `?`. Stopgap until real font rendering.
- `World::each` has a const overload, for `onDebugDraw`'s const world.

**Each frame** (`Application::render`), with the game's debug view on:

1. Only with `EngineSettings::debugBoundingBoxes` (off; F5's "Debug bounding boxes"), `drawBoundingBoxes`: boxes only round what the camera draws (`visibleDraws` in its frustum, `chosenMesh` valid at last frame's output height), since boxing every draw of a big world cost most of a frame.
2. With the same setting, `drawColliders`: every collider's query shape, orange, interpolated.
3. The readout (below), `drawCameraReadout`, the profilers.

Then, independently, `drawWindField` while `WindSettings::showField` is on (wind doc); then `onDebugDraw`; the lines go into `debugLines` and `debugScreenLines`.

**Drawing them** (`VulkanDebugLines`): `buildDebugLineVertices` (two vertices a line, colours times the HDR white level) written straight into a growing upload buffer per frame in flight (CPU stage LINES); drawn as a line list with no depth test after tone mapping, in the same rendering block, before the debug UI overlay, so colours are exact and lines show through walls. World lines through the camera; screen lines through `Mat4::orthographic` with a flipped y (it accepts flipped boxes, rejects empty ones).

## Render stats and the F4 readout

`renderer.getLastFrameStats()` returns `RenderStats` (`render/render_stats.h`): last frame's counts, each mesh once a view. GPU timings, eye adaptation and cloth timings are as late as the frames in flight.

`renderStatsLines(stats)` gives the readout's text, a line each (also the `stats` command's reply, followed by the camera and profiler lines):

| Line | Contents |
|---|---|
| `DRAWN  CULLED  CALLS` | Camera draws, culled, draw calls (prepass included) |
| `LEVELS a b c d  TRIS  CUT  PROBE  CLUSTERS`, then `TWIGS`, `FAR`, `BONES` | `drawnByLevel` (mesh first); K triangles drawn, cut-out, probe; `mesh-clusters.md`, `twig-drawing.md`, `wind-bones.md` |
| `SHADOW DRAWN  CULLED  CALLS` | Over drawn shadow views |
| `LIGHTS  IN VIEW  LIT  SHADOWED  IN MIRRORS` | Point lights; image lights only when any |
| `TILES DRAWN  CACHED  SKIPPED  SUN TO` | Shadow tiles; `sunShadowDistance` when a sun is shadowed |
| `VSM PAGES  KEPT  NEW (EVICTED)  FAILED  STALE  FREE  [COARSER]` | The sun's virtual shadow pages while on (`virtualShadowPages`; `virtual-shadows.md`); COARSER the levels' coarsening while the pool runs full |
| `VSM CASTERS n DRAWS  CLUSTERS kept/tested  TRIS nK` | Their casters, with mesh shaders (`virtualShadowCasters`) |
| `VSM DRAWN n PAIRS (n CUT-OUT)  ROOM n  TRIS nK  FULL  ATOMICS` | What was drawn into the pages (`virtualShadowDrawn`); FULL: a list past its room; ATOMICS: drawn by atomics (`virtualShadows.drawing`) |
| `VSM LEVELS (CLUSTERS:PAIRS:TRIS)  Ln c:p:t ...`, `VSM SEEN ... UNSEEN ... WHOLE ... (SWAYING ...)`, `VSM SHAPES (CLUSTERS:PAIRS:TRIS)  TERRAIN ... THINNED ...` | With `virtualShadows.countCosts`: each level's clusters, pairs and their triangles; the casters in the camera's frustum or not, those kept whole and their pairs, those kept whole for swaying; those kept whole for their shape, by kind |
| Probes | `probeStatsText`, or `PROBES -` |
| `VRAM` | `videoMemoryText` |
| `EXPOSURE ... EV  SCENE ... EV` + upscaler | `exposureText` (or `EXPOSURE FIXED`), `upscalerText` and render scale |

Then, from `Application`'s profilers (pale green, `profileDebugColour`):

| Line | Profiler | Sections |
|---|---|---|
| `CAMERA ... LOOK ...` | | `cameraText`: position and look direction, for setting up a view elsewhere |
| FRAME | `profiler` | FRAME, WORK, TICK, UPDATE, SCENE, RENDER, with worst calls |
| SCENE | `makeSceneProfiler` | `SceneStage`: BUILD, DEFORM, ATTACH, TERRAIN, STATIC, DEBUG |
| RENDER | `makeCpuStageProfiler` | The renderer's `CpuStage`s (`render/cpu_timing.h`) |
| GPU, three lines | `makeGpuProfiler` | `gpuProfileLines`: total and setup to the prepass; screen lighting from `firstLightingPass`; the picture from `firstPicturePass` |
| CLOTH | `makeClothProfiler` | Only while cloth runs on the GPU, or `clothPassesNotMeasuredText` |

- Profilers show a finished half-second window; idle sections are hidden (`hideIdle`) except the total.
- `wrapReadout` breaks a line only between items (two spaces apart), repeating `readoutUnit` (`MS`) on each wrapped line; `readoutCharactersFitting` from the screen width.
- Games add their own lines after the engine's with `writeReadout`.

**Video memory** (`render/gpu/video_memory`): `summariseVideoMemory` adds up the device-local heaps only, so system memory the GPU can reach is left out and an integrated GPU's one heap counts.

- Read at the start of each `drawScene` from `VulkanDevice::getHeapMemory` (VMA's heap budgets) after `startMemoryFrame` (makes VMA fetch the driver's figures again).
- The engine's share: each heap's `allocationBytes` (`inUseBytes`, what its resources take) and `blockBytes` (`engineBytes`, the blocks VMA took from Vulkan, up to 256 MB each, so it moves in steps and hides most changes; texture compression's 466 MB showed as 256).
- Used and budget: the driver's for the whole process with `VK_EXT_memory_budget` (`reportsMemoryBudget`, `driverReported`), VMA's estimates without it.
- Text: `VRAM used / budget MB  ENGINE n MB IN USE, m HELD  MESHES a OF b MB` (MB = 2^20 bytes; MESHES: the shared mesh buffer's meshes and pools, `gpu-scene.md`), the engine's share alone without a driver budget, `VRAM -` before a measurement.

## Tests

`debug_draw_test`, `debug_text_test`, `video_memory_test`; each system's F4 line in its own tests.
