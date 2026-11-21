# Docs index

Every doc, one line each. A doc missing here fails the docs test.

## Reference

| Doc | What it is |
|---|---|
| `README.md` | Pitch, features, install, build, controls (the packaged demo reads its `## Controls`) |
| `ROADMAP.md` | Planned and done work by phase; the current phase; work order |
| `docs/LAYOUT.md` | `src/` folders and their layers |
| `docs/BUILDLOG.md` | Why things were built, what was tried and measured: the index of its parts in `docs/buildlog/`, by phase |
| `docs/ENGINE_GUIDE.md` | Prose guide for learning the engine (for a reader with a physics background): the index of its chapters in `docs/guide/` |
| `tests/golden/README.md` | Golden reference images: layout per GPU and game |
| `assets/stars/README.md` | The star catalogue's source |

## Architecture: how each system works now

Each file's `Covers:` line names the code it describes (the pre-commit reminder reads it).

| Area | Docs |
|---|---|
| Build | `docs/architecture/build.md` (makefile, the two builds, shaders, dependencies, packaging) |
| Engine | `docs/architecture/engine-core.md` (loop, ticks, determinism, jobs, entities, input), `docs/architecture/game-interface.md` (the `Game` hooks, the frame description, culling, debug drawing, render stats), `docs/architecture/settings-and-commands.md` (settings file, F5, presets, commands, launch options) |
| Testing | `docs/architecture/testing.md` (golden images, GPU tests of shader libraries) |
| Renderer | `docs/architecture/renderer.md` (pass order, `VulkanRenderer`), `docs/architecture/gpu-scene.md`, `docs/architecture/depth.md`, `docs/architecture/gpu-driven.md`, `docs/architecture/gpu-timing.md` |
| Lights and shadows | `docs/architecture/lights.md`, `docs/architecture/clustered-shading.md`, `docs/architecture/lights-and-shadow-atlas.md`, `docs/architecture/sun-shadows.md` (cascades), `docs/architecture/contact-shadows.md` |
| Surfaces | `docs/architecture/shading.md` (BRDF, normal maps, clear coat), `docs/architecture/parallax-and-grounds.md`, `docs/architecture/glass.md` |
| Image | `docs/architecture/anti-aliasing.md` (TAA, FSR, DLSS), `docs/architecture/image-pipeline.md` (eye adaptation, bloom, tone mapping, HDR), `docs/architecture/camera-effects.md` (camera finish, motion blur, depth of field) |
| Indirect light | `docs/architecture/ambient-occlusion.md`, `docs/architecture/sky-visibility.md`, `docs/architecture/reflections.md`, `docs/architecture/reflection-probe.md`, `docs/architecture/planar-reflections.md`, `docs/architecture/mirrored-lights.md` |
| Sky and air | `docs/architecture/sky.md`, `docs/architecture/sun-moon-stars.md`, `docs/architecture/fog.md` |
| Ray tracing | `docs/architecture/ray-tracing.md`; light probes (on hold): `docs/architecture/ddgi.md`, `docs/architecture/ddgi-cascades.md`, `docs/architecture/ddgi-memory.md` |
| Content | `docs/architecture/assets.md`, `docs/architecture/materials.md`, `docs/architecture/meshes.md`, `docs/architecture/level-of-detail.md` (levels, impostors, card thinning), `docs/architecture/terrain.md`, `docs/architecture/planting.md` (plants from maps, CPU and GPU alike), `docs/architecture/wind.md` |
| Simulation | `docs/architecture/physics.md`, `docs/architecture/gameplay.md` (characters, cameras, control modes, the demo); cloth (parked): `docs/architecture/cloth.md`, `docs/architecture/cloth-solvers.md`, `docs/architecture/cloth-garments.md`, `docs/architecture/cloth-gpu.md` |
| Games | `docs/architecture/games.md`, `docs/architecture/light-lab.md`, `docs/architecture/cloth-lab.md`, `docs/architecture/forest.md`, `docs/architecture/forest-rock.md`, `docs/architecture/forest-trees.md`, `docs/architecture/forest-plants.md` |

## Research (read before building)

| Doc | Read before |
|---|---|
| `docs/research/ILLUMINATION.md` | Light probes, DDGI (phases 6d, 17) |
| `docs/research/LUMEN.md` | The software tracer or the Lumen-like layer (phase 14) |
| `docs/research/SHADOWCACHE.md` | Shadow caching for still and moving casters |
| `docs/research/PERSISTENTSCENE.md` | The persistent GPU scene's later stages |
| `docs/research/FARFOLIAGE.md` | Far foliage as volumes: Nanite Foliage's voxels and assemblies, earlier volume methods, a smaller version here |
| `docs/research/LOD.md` | Level of detail, impostors, card thinning |
| `docs/research/JUNGLE.md` | The jungle's layers, sizes and density (phase 18) |
| `docs/research/TREES.md` | Growing trees (phase 18) |
| `docs/research/VINES.md` | Lianas, climbers, epiphytes; lianas drawn by the GPU at valley scale (phase 18) |
| `docs/research/WIND.md` | The wind field and swaying |
| `docs/research/GROUNDCOVER.md` | Grass and floor cover |
| `docs/research/TEXTURECOMPRESSION.md` | Block-compressed textures, the encoder, the texture cache |
| `docs/research/OCCLUSION.md` | Occlusion culling: the depth pyramid, two-phase culling, foliage (phase 18) |
| `docs/research/GPUDRIVEN.md` | GPU-driven drawing: culling, levels and batching on the GPU, the CPU baseline (phase 18) |
| `docs/research/WORLDSCALE.md` | Cutting the GPU frame: shipped games' budgets, upscaler cost, foliage, per-view budgets, VRS, async compute (phase 18) |
| `docs/research/VALLEY.md` | Filling the valley: placement by density maps and cells, far forest tiers, sight corridors (phase 18) |
| `docs/research/STRUCTURES.md` | The structures half reclaimed: scale cues, plating, moss and streaks, vines on structures, lights, km searchlight beams in fog (phase 18) |
| `docs/research/FOGLAYERS.md` | Several fog media at once: exponential layers, pools under the canopy, later local fog volumes (phase 18) |
| `docs/research/FOGHISTORY.md` | The froxel fog's history behind fast lights (a swinging flashlight): what shipped engines do, options (phase 18) |
| `docs/research/SKY.md` | The sky's art controls, sun and moon, clouds, weather presets (phase 20) |
| `docs/research/MESHPOOL.md` | The shared mesh buffer: every mesh in one vertex and index buffer, multi-draws (phase 18) |
| `docs/research/CLOTHGPU.md` | Any cloth solver, low-frequency layer or self-collision work (phases 7, 10); yardstick numbers in Part 5 |
| `docs/research/RAGDOLLPLAN.md` | Active ragdolls (phase 8); check formulas marked as from memory |
| `docs/research/DESTRUCTION.md` | Flight and destruction (phase 15); check facts marked as from memory |
| `docs/research/ORBITAL.md` | The solar system's plan notes (phase 13); no sources yet |
| `docs/research/LATHE.md` | The lathe's plan notes (phase 14); no sources yet |
