# Game interface and the frame

Covers: `src/engine/game`, `src/scene/frame_description`, `src/scene/still_draws`, `src/scene/world_cells`, `src/scene/model_renderer`, `src/scene/culling`, `src/math/frustum`, `src/scene/level_of_detail`, `src/scene/mesh_level.h`, `src/scene/debug_draw`, `src/scene/debug_text`, `src/engine/debug_views`, `src/render/render_stats.h`, `src/render/gpu/video_memory`, `src/render/gpu/debug_line_vertices`, `src/render/vulkan/vulkan_debug_lines`, `assets/shaders/debug_line.frag`, `assets/shaders/debug_line.vert`

What a game implements, and what the engine hands the renderer each frame. The loop that calls the hooks: [engine-core.md](engine-core.md). Rules for working here: `src/engine/CLAUDE.md`.

## The `Game` hooks

`Game` (`engine/game.h`): a game sees `World`, `Input`, `Camera` and the `AssetManager`, never SDL or the GPU, so it runs headless.

| Hook | When | Notes |
|---|---|---|
| `title()` | | Window title and the game's `settings.cfg` section |
| `onInit(world, assets)` | Once, before the first frame | May keep the `AssetManager` to load more later |
| `onInput(world, input)` | Once a frame, before ticks | Turns input into commands |
| `onFixedUpdate(world, tickSeconds, simulationSeconds)` | Per tick, via `runFixedUpdate` | No `Input` on purpose: a frame of 0 or 2 ticks neither loses nor repeats a press |
| `onUpdate(world, input, frameSeconds, alpha)` | Once, after ticks | Camera, anything following interpolated poses |
| `onDebugDraw(const World&, DebugDraw&, alpha)` | Once, after the engine's debug lines | Optional |
| `getCamera()` | Each frame | The engine sets its aspect ratio from the renderer's frame |
| `wantsMouseLook()` | Each frame | Applied only when it changes |
| `getLightFocus()` | Each frame | Where lights are ranked; default the camera's position (a MOBA camera names the ground it shows) |
| `wantsDebugView()`, `setDebugView(on)` | | F4 in every game; `setDebugView` serves the `debug-view` command (false: no debug view) |
| `poseCamera(world, position, target)` | `camera` command | False by default: cannot be placed |
| `addCommands(registry, world)` | Start | The game's own commands (`forest-view`, ...) |
| `visitSettings(visitor)` | | The game's settings structs, in F5 and `settings.cfg` |
| `goldenViews()`, `prepareGoldenView(world, view)` | Golden capture | Golden images doc |

Non-virtual, set by the engine: `useJobs`/`getJobs` (`serialJobs()` until given), `useClothSolver`/`getClothSolver` (from `EngineSettings::clothOnGpu` each frame; CPU until then, so headless runs stay there).

`runFixedUpdate(game, world, tickSeconds, simulationSeconds)` saves previous transforms then calls `onFixedUpdate`; tests use it too.

## What draws: `ModelRenderer`

A component: `parts` (`RenderPart`: mesh, material, local transform) moved by the entity's one `Transform`. Flags, each copied onto its `DrawItem`s:

| Flag | Effect |
|---|---|
| `isVisible()` | Off: no draws built |
| `castsShadows` | Blocks light in shadow maps |
| `shadowOnly` | Drawn into shadow maps only; no camera view, no ray: a cheap stand-in caster |
| `shadowCascades` | Casts into the sun's nearest n cascades only (0 all) |
| `isStatic` | Part of the level: probes and the sky bake treat it as solid |
| `hidden` | Drawn by no view, no shadow, no ray, but still static level (the forest's walked-on ground) |
| `isStill()` | Never moves or changes: draws kept between frames (below). Independent of `isStatic` |
| `followsGround` | Lifted onto the terrain by the vertex shaders |
| `sway` | `WindSway`: how the vertex shaders bend it |

Shown and still are private: chosen before joining a world by `withVisible` and `withStill` (callable only on a value about to be moved in), then changed only by `setRenderVisible(world, entity, on)` and `setRenderStill`, which announce a change (`markRenderChanged`). So the still draws never walk the renderers to find them.

## `FrameDescription`

Plain data, no GPU types (`scene/frame_description.h`); the whole frame at once lets the renderer draw the scene many times (shadow tiles, captures) and cull per view.

- `camera` (a copy), `lighting` (`collectLighting` at alpha), `draws`, `lightFocus`, `frameSeconds`.
- `stillDraws`, `builtStillDraws`, `stillVersion`, `stillPreviousVersion`: the kept still draws (below).
- `debugLines`, `debugScreenLines`.
- `deformedMeshes`: each deforming mesh's shape this frame (deforming meshes doc).
- `staticLevel`, `staticLevelVersion`, `staticAlbedo`: the sky visibility grid's triangles (`engine/static_geometry`, while `skyVisibility.enabled`).
- `terrainPatches`, `terrainHeights`, `terrainHeightsVersion`: terrain doc.
- `wind`, `windAnchors`: the first `Wind` and `WindAnchors` placed (wind doc).

`DrawItem`: mesh, model matrix, material, `castsShadows`, `isStatic`, plus fields filled later. Meanings per field: the header.

| Field | Filled by |
|---|---|
| `previousModel` | `buildFrame` from `MotionHistory` (none on a first frame) |
| `shadowOnly`, `shadowCascades`, `hidden`, `followsGround`, `sway` | `buildEntityDraws` from the `ModelRenderer` |
| `shape` | `attachShapes`: a deformed mesh's fingerprint, for the shadow cache |
| `bounds` | `attachBoundingSpheres` |
| `flatFaces` | `attachFlatFaces` (planar mirrors choose from them) |
| `hangingEntry`, `thinningEntry` | `attachVertexEntries`, from the `VertexEntries` `uploadMesh` returned |
| `levels`, `hiddenBelowError`, `levelBounds` | `attachMeshLevels` |
| `terrainPatch` | `appendTerrainDraws` (-1 for other draws) |

## Building a frame

`Application::render` → `describeFrame(alpha, frameSeconds)`, each step timed on the readout's SCENE line (`SceneStage`):

| Step | Work |
|---|---|
| (TERRAIN, part 1) | `selectTerrainNodes` per `Terrain` first, so `buildFrame` reserves room (`laterDraws`) and appending patches never moves the list |
| BUILD | `buildFrame(world, camera, alpha, &motionHistory, jobs, laterDraws, still)`; `lightFocus` from the game; `clearProbeEvents`, `clearRenderChanged` (one-frame components) |
| DEFORM | Each `DeformingMesh` shaped, a job each (`deformMesh`, or `gpuClothDeformedMesh`: bounds only, grown for points `framesInFlight + 1` frames late); `boundsByMesh` updated; `attachShapes` on built draws |
| ATTACH | On `builtStillDraws` ranges and the moving draws only: bounding spheres, flat faces, vertex entries, mesh levels, `liftGroundDraws` (last terrain's heights) |
| TERRAIN | `appendTerrainDraws`; the frame takes the terrain's heights and version |
| STATIC | `StaticGeometry::update` (a hash of the static draws) |
| DEBUG | The debug views below and `onDebugDraw` |

After `renderer.render`, `giveBackStillDraws` returns the list. Golden captures and screenshots use the same `describeFrame` at alpha 1.

**`buildFrame`:** draws for every `ModelRenderer` with a `Transform` and `visible`, a draw per part, sized exactly first (prefix sums per entity), then filled in parallel (`buildEntityDraws`, 256 entities a piece) so the order is fixed. Pose: `getRenderTransform` at alpha, times the part's local transform.

**`MotionHistory`** (owned by `Application`, render-only): each entity slot's part matrices in two record sets taking turns by frame parity, a record counting as last frame's only by its frame stamp, so nothing is cleared or allocated between frames; keyed by slot and generation, so a reused slot starts afresh. `reserveSlots` before the parallel fill (each entity writes only its own slot).

## Still draws

`StillDraws` (`scene/still_draws`, owned by `Application`): still entities' draws kept between frames, so a frame builds only what moves. The renderer's matching cache of packed records (`StillInstances`): `gpu-scene.md`.

- `update(world, alpha, jobs)` returns the kept draws first; `buildFrame` puts them at the front (`stillDraws` slots) and builds the moving draws after. `giveBack` hands the list (and its memory) back once drawn.
- **Laid out by cell** (`scene/world_cells`, GPU-driven drawing's stage 7a): the ground cut into `CellSettings::size` squares (128 m, `engine.cells.*`), keyed by 64-bit column and row (`worldCellOf`, from the entity's transform). Each cell a block of slots (`cellBlockSlots`: its draws plus `spareShare` 0.25, at least `fewestSlots` 64, in whole chunks of `chunkSlots` 64, so a slot's chunk is slot / 64 for the GPU's chunk culling, `gpu-driven.md`), cells by key, a cell's entities in Morton order (`cellMortonCode`: x and z across the cell, 16 bits each, interleaved; sorted again on every rebuild of the cell, so runs of slots and chunks stay close on the ground); spare slots hold `emptyDrawSlot()` (no mesh, hidden, no shadow, unknown bounds), which every view, ray and list passes over (`isEmptyDrawSlot`). Blocks come from `SlotBlocks`: first free run that fits, else the end.
- **Only changes looked at:** the world's change logs (`engine-core.md`) for `ModelRenderer`, `Transform` and `DeformingMesh`, plus the entities marked `RenderChanged`, each once in entity order. Each entry has a kind (none, kept, moving); an entity whose kind changed joins or leaves a cell or the moving list. `update` returns the moving renderers' storage places, sorted (the order a walk gives), and `buildFrame` builds only those. Cost: the changes plus the moving entities, not the world.
- **A cell rebuilt** when a still entity joins or leaves it (added, destroyed, hidden or shown, stilled or not, Transform or deforming mesh gained or lost, an announced move into another cell) or an announced change alters its part count: only that cell's block. A cell outgrowing its block moves, its old block emptied; both are in `builtStillDraws`. The slot count only grows between full layouts; the built ranges cover every new slot.
- **Laid out whole** (one walk over every renderer, the logs turned on) when the world was not logging (the first frame, or after `stopLogging`, which the engine calls while `keepStillDraws` is off), when a frame was not given back, with other cell settings, or after `invalidate` (the engine calls it when `AssetManager::getLevelsVersion` changes, since draws hold spans of the levels).
- **Rebuilt in place:** an entity marked by `markRenderChanged` (a one-frame `RenderChanged` component; once per entity inside a walk) that stays in its cell. `builtStillDraws` lists the rebuilt ranges, sorted and joined (`joinDrawRanges`).
- `FrameDescription::stillChunkSlots` carries the chunk size to the renderer; `stillCells`, `stillEmptySlots` feed F4's `CELLS n  SLOTS m  UPLOADED k KB` line (with the renderer's copied bytes); DRAWN/CULLED leave the empty slots out.
- An entity with a `DeformingMesh` is never kept.
- A still draw's `previousModel` is its own matrix: an announced move is a jump (no motion blur or smear).
- Each update has a `version` naming the previous one, so the renderer knows whether only `builtStillDraws` changed.
- Debug builds: `verify` checks 256 kept entities a frame (taking turns) and throws `std::logic_error` on one that moved or changed part count unannounced, or is no longer still and shown (its renderer replaced whole).
- `EngineSettings::keepStillDraws` (on, `engine.keepStillDraws`) switches it off for measuring.

## Culling

- `Frustum::fromClipMatrix(viewProjection, ClipDepth)` (`math/frustum`): six inward unit planes from the matrix rows; `NegativeOneToOne` for `Mat4::perspective`'s clip space, `OneToZero` for the renderer's reversed-Z matrices.
- `intersectsSphere` and `mayIntersect` may say true for a near miss past a corner, never false for an overlap; `corners` gives the eight.
- `boundingSphere(localBox, model)`: the box's centre through the matrix, half-diagonal times the largest axis scale. Radius below 0 = unknown, never culled.

| Function | Gives |
|---|---|
| `visibleDraws(draws, frustum)` | Camera views: skips `shadowOnly` and `hidden` |
| `shadowCasters(draws, frustum)` | Opaque casters in a light's view; never use the camera's frustum (off-screen casters shadow the view) |
| `castersInReach(draws, reach)` | Casters touching a light's sphere of reach, before a point light's six faces |
| `castersForCascade(draws, casters, cascade)` | Drops casters whose `shadowCascades` stops before this sun cascade |
| `castersAtLeast(draws, candidates, radius)` | The far shadow map's: drops what is too small for its texels |
| `glassInReach(draws, reach)`, `glassInReach(draws, candidates, reach)` | Glass a light reaches, for coloured shadow maps (any `castsShadows`); of a list only (the GPU culling's cached glass) |

Glass is never a solid caster, nor a solid caster glass. Which views call these: renderer.md.

## Level of detail: choosing

Each view draws the coarsest of a mesh's levels whose error covers at most `pixelError` pixels. Naming levels, their options, impostors and card thinning: `level-of-detail.md`; research: `docs/research/LOD.md`.

- **Levels:** `MeshLevel` {mesh, `errorMetres`, `sharesVertices`}, coarsest last, at most three (`meshLevelSlots` = 4 with the mesh); a last level without a mesh draws nothing.
- **Attaching:** `attachMeshLevels` gives each draw its levels' span and options, `chooseBounds` placed by the draw's matrix into `levelBounds`. `DrawItem::mesh` stays the mesh itself for bounds, rays, flat faces, the sky bake.
- **`LevelView`:** eye, `pixelsPerMetre` = height / (2 tan(fov / 2)), `pixelError`, `enabled`, `impostors`, `orthographic` (the sun's tiles: distance does not count). `levelView(eye, heightPixels, fov, settings)`.
- **`errorInPixels`:** e · pixelsPerMetre / d (infinite at d ≤ 0); e · pixelsPerMetre orthographic.
- **`chooseLevel`:** walks the levels until one does not fit, or (impostors off) one has no mesh. Off: level 0.
- **`levelDistance`:** to the sphere's surface (0 inside), so a big tree's near side is never coarse. `drawLevel` uses `levelBounds` when set, else `bounds`; unknown bounds keep the mesh.
- **`chosenMesh`:** the drawn mesh, or an invalid handle where the draw draws nothing (an impostor too near, levels or impostors off; a tree past its last level). Batching leaves those out.
- **`levelsChosen`:** counts per level for the readout's LEVELS.
- **`thinningErrorPerMetre`:** pixelError / pixelsPerMetre, the allowance card thinning spends (0 orthographic or off); card thinning doc.
- The camera chooses by its **output** height (`RenderStats::outputHeight`), not the render height, so upscaling does not pull switches closer. Other views' choices (velocity pass, probe faces, mirror, shadow tiles by their texels): renderer.md.
- **Switching** is instant; TAA softens it.
- **Settings:** `LevelOfDetailSettings` (`RenderSettings::levelOfDetail`, F5 Level of detail): `enabled`, `pixelError` (2), `impostors`; live. `validateLevelOfDetail` rejects a budget not above 0.

## Debug drawing

`DebugDraw` (`scene/debug_draw`): immediate-mode lines, cleared every frame. Colours linear.

- World lines: `line`, `box(bounds, transform)`, `sphere` (three circles), `capsule`, `shape` (any collision `Shape`).
- `screenLine`: window pixels, top-left origin.
- Readout layout: `takeReadoutLine` hands out the next text line (engine's first, then the game's); `setScreenWidth` (the last frame's `outputWidth`) for wrapping.
- `debugText`/`debugTextWidth` (`scene/debug_text`): stroke font on a 4 x 6 grid, A-Z (lower case as upper), 0-9, space, `: - / . ?`; anything else as `?`. Stopgap until real font rendering.
- `World::each` has a const overload, for `onDebugDraw`'s const world.

**Each frame** (`Application::render`), with the game's debug view on:

1. Only with `EngineSettings::debugBoundingBoxes` (off; `debugBoundingBoxes`, F5's "Debug bounding boxes": in a forest they hide the readout), `drawBoundingBoxes`: boxes only round what the camera draws (`visibleDraws` in its frustum, `chosenMesh` valid at last frame's output height), since boxing every draw of a big world cost most of a frame.
2. With the same setting, `drawColliders`: every collider's query shape, orange, interpolated.
3. The readout (below), `drawCameraReadout`, the profilers.

Then, independently, `drawWindField` while `WindSettings::showField` is on (wind doc); then `onDebugDraw`; the lines go into `debugLines` and `debugScreenLines`.

**Drawing them** (`VulkanDebugLines`): `buildDebugLineVertices` (two vertices a line, colours times the HDR white level) written straight into a growing upload buffer per frame in flight (CPU stage LINES); drawn as a line list with no depth test after tone mapping, in the same rendering block, before the debug UI overlay, so colours are exact and lines show through walls. World lines through the camera; screen lines through `Mat4::orthographic` with a flipped y (it accepts flipped boxes, rejects empty ones).

## Render stats and the F4 readout

`renderer.getLastFrameStats()` returns `RenderStats` (`render/render_stats.h`): last frame's counts, each mesh counted once per view. GPU timings, eye adaptation and cloth timings are as late as the frames in flight.

`renderStatsLines(stats)` gives the readout's text, a line each (also the `stats` command's reply, followed by the camera and profiler lines):

| Line | Contents |
|---|---|
| `DRAWN  CULLED  CALLS` | Camera draws, culled, draw calls (prepass included) |
| `LEVELS a b c d  TRIS  CUT  PROBE` | `drawnByLevel` (mesh itself first); thousands of triangles drawn, of cut-outs, of reflection probe faces this frame |
| `SHADOW DRAWN  CULLED  CALLS` | Over drawn shadow views |
| `LIGHTS  IN VIEW  LIT  SHADOWED  IN MIRRORS` | Point lights; image lights only when any |
| `TILES DRAWN  CACHED  SKIPPED  SUN TO` | Shadow tiles; `sunShadowDistance` when a sun is shadowed |
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

`frame_description_test`, `still_draws_test`, `world_cells_test`, `culling_test`, `level_of_detail_test`, `debug_draw_test`, `debug_text_test`, `video_memory_test`.
