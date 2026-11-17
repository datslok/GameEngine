# Wind

Covers: `src/scene/wind`, `src/render/gpu/wind`, `src/render/gpu/wind_shared.h`, `src/render/vulkan/vulkan_wind`, `assets/shaders/wind.glsl`, `assets/shaders/wind_data.glsl`, `assets/shaders/wind_field.comp`

A wind force field of springs round the camera, after Horizon Zero Dawn's, that every vertex shader bends growing and hanging things by. Research and plan: `docs/research/WIND.md`. Maths in `render/gpu/wind`, mirrored in `wind.glsl`. The forest's use (sway groups, lianas' anchors): [forest.md](forest.md), [forest-trees.md](forest-trees.md), [forest-rock.md](forest-rock.md).

## The wind (`scene/wind`)

`Wind` component (the first placed counts; `FrameDescription::wind`): `towardsDegrees` (from +x towards +z), `speed`, `gustStrength`/`gustSize`, `turbulence`/`turbulenceSize`. Defaults and units: `scene/wind.h`.

`windVelocity(wind, place, seconds)`:

- Gusts: speed · max(0, 1 + gustStrength (2g - 1)), g a value noise (`windNoise`: `pcg3d` lattice, smoothstep) read upwind by the wind's travel, so gusts travel with it (frozen turbulence), changing shape `WIND_GUST_EVOLUTION` (0.25) as fast.
- Plus sideways turbulence: turbulence · speed · (2s - 1) from a second noise offset by `WIND_TURBULENCE_OFFSET`.

## The springs

Springs, not the wind, are what things sample: they lag a gust, overshoot and settle as stems do.

- **Windows:** `WIND_CASCADES` (2) of `WIND_CELLS` (64) cells square round the camera: `nearCellMetres` 2 (128 m) and `farCellMetres` 16 (1 km).
- **Per cell:** `WIND_SPRINGS` (6): trees, plants, grass (`WIND_CATEGORY_*`), then hanging things in three bands, short, medium, long (`WIND_HANGING_BANDS`, from `WIND_SPRING_HANGING_SHORT`). Each band's `WindSpring` (frequency, damping, response) in `WindSettings::springs`; the hanging bands' frequencies must fall.
- **Step:** `windSpringTarget` = velocity · |velocity| · response (drag goes with speed squared); `windSpringStep`: x'' = ω² (target - x) - 2ζω x', semi-implicit Euler in substeps of at most `WIND_SUBSTEP_TURN` (0.5) radians, at most `WIND_MOST_SUBSTEPS` (8), the frame's step capped at `WIND_LONGEST_STEP` (0.1 s) so a hitch never throws them.
- **Wrapping:** a texel holds the window's cell equal to it modulo `WIND_CELLS` (`windCellOfTexel`), so a camera move copies nothing. `windWindowAround` places each window; a cell new to the window (`windCellInWindow`), or every cell when last frame's windows are unknown, starts at rest.

## On the GPU (`VulkanWind`)

- Two `R16G16B16A16_SFLOAT` 2D arrays of 64 x 64 x 12 (a layer per cascade and spring; xy displacement, zw velocity), taking turns.
- `prepare` (before the scene set is written) packs the record (`packWindField`, `GpuWindField`, 304 bytes) and turns the images, so binding 38 is the image this frame's pass writes and 39 last frame's.
- `step`: `wind_field.comp` (8 x 8 groups, `WIND_GROUP_SIDE`) writes this frame's springs from last frame's, then `ImageState::VertexRead`. Pass `GpuPass::Wind` (WIND in F4); its place in the frame: [renderer.md](renderer.md).
- Scene set 1: record 37, springs 38 and 39 (a repeating linear sampler), hanging vertices 40, anchors 41 ([gpu-scene.md](gpu-scene.md)).
- No `Wind` in the frame, or `WindSettings::enabled` off: the pass does not run, the record says off, the springs are forgotten.

## Swaying

**What sways:** `ModelRenderer::sway` (`WindSway`, copied to `DrawItem::sway`):

| Field | Meaning |
|---|---|
| `kind` | `SwayKind`: `Still` (default), `Tree`, `Plant`, `Grass`, `Hanging` (the field's categories in order) |
| `height` | What the bend is measured against, in the mesh's units, so it scales with the object |
| `bend` | Lean per radian of the spring |
| `flutter` | Metres leaves shiver per radian of lean |
| `swing` | Hanging vertices' swing per radian and metre of arm (below) |

`packSway` → `GpuInstance::sway` (category, -1 still; height; bend; flutter); `swing` rides in `GpuInstance::hanging[1]`.

**Where the spring is read** (`windOwnSway`, `windSpringAt` in `wind_data.glsl`):

- Trees, plants, hanging: at the foot (the model's origin), so everything with one pose (a tree's bark, leaves, stand-ins, climbers, epiphytes) bends as one.
- Grass: at the ground below each vertex ((x, 0, z) in the mesh), so gusts roll across a meadow.
- The near window, passing into the far one over its last `WIND_BLEND_CELLS` (8) cells (`windWindowShare`); outside both, nothing.

**The offset** (`windSwayOffset`):

- Lean = spring · bend, softly held under `WIND_MOST_LEAN` (`windSoftLean`: lean / (1 + |lean| / 0.5)), times the fade (`windSwayFade` from `swayFadeStart` to `swayFadeEnd` by the foot's distance).
- Main bend: a vertex at height h moves sideways by lean · h² / height (a cantilever), then back to its distance from the foot, so it bends rather than stretches.
- Branches bob by `WIND_BRANCH_SHARE` of the lean times their distance from the trunk at `WIND_BRANCH_HERTZ`, phase changing every `WIND_BRANCH_METRES`.
- Leaves flutter (`windFlutterOffset`) by flutter · lean in three directions at their category's `WindFlutter` (hertz, and metres over which the phase turns once: `windPhase`, `windFlutterRate`). A long wave keeps a frond or tuft in step.
- Bob and flutter grow with the height share, so a foot never moves. Phases come smoothly from the world place, so nothing tears. Normals are not turned (small angles).
- Impostor cards do not sway, so the fade should end before a tree turns into its card (the forest's: about 1 km).

**Every view the same:** `triangle.vert` (prepass, main pass, captures) and `motion.vert` add `windSwayOfVertex` after ground following ([terrain.md](terrain.md)). The velocity pass redraws every draw that `drawSways` (has a sway, partly within the fade's end) every frame, its last position from last frame's springs, windows and time (`GpuWindField::sway`: on, fade start and end, last time), so TAA, FSR and DLSS see the motion. Ray tracing sees the still shapes.

## Swaying shadows

- `shadow.vert` and `shadow_cut_out.vert` sway only in tiles that sway: `GpuShadowView::options.x`, set by `tileSways` for the sun's nearest `swayShadowCascades` (1).
- Such a tile's fingerprint takes the frame's number while something in it sways (`swayingShadowSignature`), so it is redrawn every frame; other cascades and lamps keep still, cached shadows, as Horizon Zero Dawn's do. Shadow cache: [lights-and-shadow-atlas.md](lights-and-shadow-atlas.md).

## Hanging things

Lianas, roots, vines and moss move with what they hang from and swing below it.

**Per vertex** (`IndexedMesh::hanging`: empty, or one `HangingVertex` a vertex):

- `first`, `second`: anchors (the same twice for one). An index into the frame's `WindAnchors`, or `windAnchorFixed` (rock, the ground: never moves) or `windAnchorOwn` (the object's own sway at the vertex: a root in its tree's frame).
- `along`: share of the way from first to second (their motions blended by it).
- `arm`: how far it hangs below the chord (or its one anchor), in metres.
- `swingLength`: the pendulum its strand swings as: a strand from one anchor its whole length, one between two its deepest sag.
- `appendHanging` keeps the list one a vertex (vertices without entries get the default: own sway, no arm); `AssetManager::addDrawnMesh` pads the end.
- Entries are found by the draw's own mesh: see [level-of-detail.md](level-of-detail.md), A mesh's levels.

**Anchors** (`WindAnchor`: a tree's foot, height, scale, bend and the point; the `WindAnchors` component, first placed counts, shared without copying): `windAnchorOffset` moves an anchor exactly as its tree's own vertex there would (the trees' spring at its foot, no flutter), so what hangs from a crown stays on it.

**The swing's speed:** a hanging chain's slowest swing is sqrt(g / L) / `WIND_CHAIN_PERIOD` Hz (5.2255: 2π over half J0's first zero), L floored at `WIND_SHORTEST_SWING` (`windChainHertz`). `windChainShares` blends the two bands round that frequency by log frequency (all short above the short band, all long below the long); `windChainSpringAt` reads only bands with a share. A gust kicks a short root at once and leans a long vine slowly, which swings on after.

**The motion** (`windSwayOfVertex` for a vertex with an entry):

1. Each anchor's motion (`windAnchorMotion`): 0 fixed, the own sway, or `windAnchorOffset`.
2. `windSwingOffset`: the hanging spring at the vertex times `swing`, softened and faded, is a lean it swings out by over its arm, rising to keep its length (a pendulum's arc); leaves flutter at the hanging category's rate (not when an anchor is own: the own sway fluttered already).
3. `windHangingOffset`: anchors blended by `along`, plus the swing.

A mesh without entries of kind `Hanging` sways as its own object on the short band.

**On the GPU:**

- `packHangingVertex`: three words (anchors as two 16-bit halves; along and arm as half floats; swing length as a half float), binding 40, read by `gl_VertexIndex`. `uploadMesh` returns the mesh's entry (`VertexEntries`), `attachVertexEntries` gives each draw `DrawItem::hangingEntry` → `GpuInstance::hanging[0]`. Copied whole when new ones arrive (`updateVertexRecords`).
- Anchors: `packWindAnchor` → `GpuWindAnchor` (48 bytes), binding 41, written each frame.

## Debug view

`WindSettings::showField` (off): `drawWindField` (`engine/debug_views`) draws 9 x 9 arrows 4 m apart round the camera on the drawn ground (`terrainSurfaceHeight`), each half a second of wind, white to red towards 15 m/s.

## Settings

`WindSettings` (`RenderSettings::wind`, F5 Wind, `engine.render.wind.*`), live: `enabled`, cell sizes, `springs`, `flutters`, `showField`, `sway`, `swayFadeStart`/`End` (80, 400 m), `swayShadowCascades`. Meanings and defaults: `render/gpu/wind.h`; `validateWind` lists what it rejects.

## Tests

`wind_test` (noise, velocity, springs, windows), `wind_sway_test`, `wind_hanging_test`; GPU: `wind_gpu_test`.
