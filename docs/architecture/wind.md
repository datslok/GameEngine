# Wind

Covers: `src/scene/wind`, `src/scene/sway_bones`, `src/scene/liana_shape`, `src/scene/liana_shared.h`, `assets/shaders/liana.glsl`, `assets/shaders/liana_data.glsl`, `src/render/gpu/wind`, `src/render/gpu/wind_shared.h`, `src/render/vulkan/vulkan_wind`, `assets/shaders/wind.glsl`, `assets/shaders/wind_data.glsl`, `assets/shaders/wind_field.comp`, `src/render/gpu/wind_bones`, `src/render/gpu/wind_bones_shared.h`, `assets/shaders/wind_bones.glsl`, `assets/shaders/quat.glsl`, `src/render/gpu/bone_records`, `src/render/gpu/bone_records_shared.h`, `src/render/gpu/bone_poses`, `src/render/gpu/bone_poses_shared.h`, `assets/shaders/bone_poses.glsl`, `assets/shaders/bone_pass.glsl`, `assets/shaders/bones_claim.comp`, `assets/shaders/bones_pose.comp`, `src/render/vulkan/vulkan_bone_posing`

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
- Leaves flutter (`windFlutterOffset`) by flutter · lean in three directions at their category's `WindFlutter` (hertz, and metres over which the phase turns once: `windPhase`, `windFlutterRate`). A long wave keeps a frond, tuft or patch of crown in step: the trees 1 Hz over 8 m (faster and finer, leaves darting through the sun flecks sparkled), plants 1.5 Hz and grass 1.2 Hz over 8 m, hanging things 3 Hz over 0.5 m.
- Bob and flutter grow with the height share, so a foot never moves. Phases come smoothly from the world place, so nothing tears. Normals are not turned (small angles).
- Impostor cards do not sway, so the fade should end before a tree turns into its card (the forest's: about 1 km).

**Every view the same:** `triangle.vert` (prepass, captures) and `triangle_motion.vert` (main pass) add `windSwayOfVertex` after ground following ([terrain.md](terrain.md)). The main pass works out a vertex's last position from last frame's springs, windows and time (`GpuWindField::sway`: on, fade start and end, last time) wherever the wind offsets it now, so TAA, FSR and DLSS see the motion ([anti-aliasing.md](anti-aliasing.md)). Ray tracing sees the still shapes.

## Swaying shadows

- `shadow.vert` and `shadow_cut_out.vert` sway only in tiles that sway: `GpuShadowView::options.x`, set by `tileSways` for the sun's nearest `swayShadowCascades` (1), and for a lamp's tiles (the flashlight's) whose reach comes within `swayFadeEnd` of the camera (`swayLocalShadows`).
- Such a tile's fingerprint takes the frame's number while something in it sways (`swayingShadowSignature`), so it is redrawn every frame; the other cascades keep still, cached shadows, as Horizon Zero Dawn's do. Shadow cache: [lights-and-shadow-atlas.md](lights-and-shadow-atlas.md).
- A lamp's tile drawn still while its leaves sway blackens them: each leaf sways in front of its own still shadow. A moving flashlight redraws its tile every frame anyway, so swaying it costs only while it stands still.

## Hanging things

Lianas, roots, vines and moss move with what they hang from and swing below it.

**Per vertex** (`IndexedMesh::hanging`: empty, or one `HangingVertex` a vertex):

- `first`, `second`: anchors (the same twice for one): `windAnchorFixed` (rock, the ground: never moves) or `windAnchorOwn` (the object's own sway at the vertex: a root in its tree's frame).
- `along`: share of the way from first to second (their motions blended by it).
- `arm`: how far it hangs below the chord (or its one anchor), in metres.
- `swingLength`: the pendulum its strand swings as: a strand from one anchor its whole length, one between two its deepest sag.
- `appendHanging` keeps the list one a vertex (vertices without entries get the default: own sway, no arm); `AssetManager::addDrawnMesh` pads the end.
- Entries are found by the draw's own mesh: see [level-of-detail.md](level-of-detail.md), A mesh's levels.

**Anchors on trees** (`GpuWindAnchor`: a tree's foot, height, scale, bend and the point; built from a liana's record, `lianaWindAnchor`): `windAnchorOffset` moves an anchor exactly as its tree's own vertex there would (the trees' spring at its foot, no flutter), so what hangs from a crown stays on it.

**Lianas** (`lianaSwayOffset`, `liana_data.glsl`): no hanging vertices; each vertex's share along, arm below the chord and the swing's length (the deepest sag) come from its record's curve, its anchors from its trees in the record (a rope's far end fixed), then as any hanging vertex below. An ivy card's corners all sway as its stem (`lianaDrawnVertex`'s `swaysAt`), so the card swings whole.

**The swing's speed:** a hanging chain's slowest swing is sqrt(g / L) / `WIND_CHAIN_PERIOD` Hz (5.2255: 2π over half J0's first zero), L floored at `WIND_SHORTEST_SWING` (`windChainHertz`). `windChainShares` blends the two bands round that frequency by log frequency (all short above the short band, all long below the long); `windChainSpringAt` reads only bands with a share. A gust kicks a short root at once and leans a long vine slowly, which swings on after.

**The motion** (`windSwayOfVertex` for a vertex with an entry):

1. Each anchor's motion (`windAnchorMotion`): 0 fixed, the own sway, or `windAnchorOffset`.
2. `windSwingOffset`: the hanging spring at the vertex times `swing`, softened and faded, is a lean it swings out by over its arm, rising to keep its length (a pendulum's arc); leaves flutter at the hanging category's rate (not when an anchor is own: the own sway fluttered already).
3. `windHangingOffset`: anchors blended by `along`, plus the swing.

A mesh without entries of kind `Hanging` sways as its own object on the short band.

**On the GPU:**

- `packHangingVertex`: three words (anchors as two 16-bit halves; along and arm as half floats; swing length as a half float), binding 40, read by `gl_VertexIndex`. `uploadMesh` returns the mesh's entry (`VertexEntries`), `attachVertexEntries` gives each draw `DrawItem::hangingEntry` → `GpuInstance::hanging[0]`. Copied whole when new ones arrive (`updateVertexRecords`).
- Lianas' records: binding 41 (`LianaShape`, 128 bytes with the ivy's, `writeLianaShapes`: a slot's block copied when it is replaced). A draw's `hanging.w` is its record + 1, with `LIANA_IVY_BIT` (`scene/liana_shared.h`) for its ivy.

## Bones (phase 21 stage 3c, being built)

Trees will move by bones instead of `windSwayOffset` (research: `docs/research/LEAFGEOMETRY.md`, Wind with valid bounds). Built so far: the bones and bindings (3c-i), their motion's maths (3c-ii), every tree mesh bound and the records on the GPU (3c-iii-a), the bones pass posing them each frame (3c-iii-b); nothing reads the poses yet (3c-iii-c).

- `SwayBone` (`scene/sway_bones`): pivot, tip, parent (before it in the list, -1 a root), level (0 the trunk, +1 a side chain), the line's radius at its pivot; its chain's first bone, length and radius at its start (`chainStart`, `chainLength`, `chainRadius`).
- `SwayBinding`: a vertex's bone and its weight, the rest its parent's (two-bone blending, GPU Gems 3 ch. 6), so joints never open.
- `appendSwayChain`: a line cut into whole bones of one length near `boneMetres`, each the one before's child; `swayArcOfPoint` (how far along a line a point lies); `swayChainBinding` (the bone at an arc, weight rising from 0 at its pivot over its first `blendShare`, a root's whole); `swayBonesOrdered`.
- Trees: `treeBones` (`games/forest/forest_trees`) cuts each chain, chains taken by first node so parents come first, a side chain's first bone the child of the bone holding its start node; `TreeMeshes::bones`; `bark.swayBindings` (each bark vertex by its own chain's arc), its levels by `simplifyMesh`; the cards whole, each by its node (`appendClusterCards` given the nodes' bindings, `nodeOrNearest`), as `PlacedTwig::binding` (its node's; a filler its nearest node's); climbers and epiphytes by the nearest bark (`swayLines`, `bindToTreeBones`, `swayNearestBinding`). `ForestTreeSettings::boneMetres` 2 m, `boneBlendShare` 0.5 (F5 Forest, at start-up).

Motion (`render/gpu/wind_bones` and `wind_bones.glsl`, mirrored; quaternions in `quat.glsl`, mirroring `math/quat.h`); all in one frame, metres, the tree's lean given in it:

| Function | Does |
|---|---|
| `windBoneShare` | the bone's share of the lean: `2 L / H min((r_foot / r)^k, most)`; an untapered trunk bends as `windSwayOffset`'s parabola |
| `windBranchHertz` | its chain's swing, a cantilever's first mode `0.5596 (r / 2) sqrt(E / rho) / L^2` (`WIND_BONE_FIRST_MODE`), clamped |
| `windBranchPhase` | its chain's phase, `pcg3d(tree seed, chain start)`: a branch's bones swing together |
| `windBoneTurn` | its own turn (rotation vector) from its rest direction: `s (d x W)(1 + a sin t) + s b (W - d (d.W)) cos t`, an ellipse along and across the wind; none in still air |
| `windBonePose` | `T(x) = q x + t` from its parent's, parents first: `T(p_b) = T_parent(p_b)`, so joints never open |
| `windBlendedPlace`, `windBlendedDirection` | a vertex, a normal: its two bones' images blended by its weight |
| `windPosedSphere`, `windSphereUnion` | a cluster's bound: its rest sphere posed by each bone of its run and their parents, merged; no pad |
| `windBoneOwnTurn` | a bone's own turn in its tree (`WindBoneShape`, `WindTreeShape`): its share, its chain's hertz in metres and phase |
| `windTreePoses` | (C++ only) every pose of a tree in list order, for tests and checks |

`WindBoneMotion` (stiffness exponent 3, most 8, wood speed 3000 m/s, branches 0.2-3 Hz, swing along and across 0.3) is `WindSettings::bones` (F5 Wind, Bones), in the field's record (`GpuWindField::bones`, 336 B).

On the GPU (`render/gpu/bone_records`, 3c-iii-a):

- A part moved by bones carries its look's `SwaySkeleton` (bones, height in mesh units): `RenderPart::skeleton`, `DrawItem::skeleton`; the forest gives it every swaying tree part (bark, leaves, shadow stand-ins, twigs, climbers, epiphytes), not the impostor card. The placed table lists them once (`PlacedTable::skeletons`), a part's `GpuPlacedPart::unused[1]` its number plus 1.
- `packSwaySkeletons`: `GpuSwayBone` (48 B; parent and chain start within its skeleton), `GpuSwaySkeleton` (32 B: run, height, foot radius, its chain levels, its lean part), `GpuSwayChain` (first bone, count) by level (trunk, its branches, theirs) with each level's start; refuses a chain not a run of children of one level, or starting on a bone not of a lower level.
- Vertex bindings beside the vertex pool, a word a pool vertex (`packSwayBinding`: bone low 16 bits, weight unorm16 high; `SWAY_UNBOUND` otherwise), read by the vertex's place in the pool, so levels with their own vertices and cluster levels need no per-mesh entry; `placeSwayBindings` at upload, the list ending at the last bound mesh (scene binding 48). 4 B a pooled vertex beside its 48 B.
- Twigs: `TwigInstance::binding`, `packTwigBinding`, a word a twig beside the twigs (`VulkanTwigDrawing::getTwigBindings`, scene binding 49).

**The bones pass** (3c-iii-b; `render/gpu/bone_poses` mirroring `bone_poses.glsl`, `render/vulkan/vulkan_bone_posing`, `bones_claim.comp`, `bones_pose.comp`, GPU timer BONES): after [PLACED] and [WIND] (moved there), before the twigs' and cluster passes, while things sway and placed parts carry skeletons.

- **Swaying views:** the camera (placed view 0) and the shadow tiles drawn swaying (`tileSways`); probe captures draw trees still.
- **Claim** (a workgroup a group of a bound part (`boneGroups`) and swaying view, a thread an entry): a plant nearer than `swayFadeEnd` (`boneTreeWanted`) claimed by an `atomicExchange` of the frame's stamp in its word pair (stamp, tree; 8 B a held plant, never cleared after it is made); the claimer takes the next tree (`GpuBoneTree` 32 B, up to `BONE_MOST_TREES` 65536) and its bones' room (`boneTreeFirstPose`: two poses a bone from twice the bones asked before, or `BONE_NO_POSE`, standing still), raising the pose pass's dispatch.
- **Pose** (indirect, a workgroup a tree): its lean now and last frame (`windFadedLean` of the trees' spring at its foot by its skeleton's lean part's bend (`skeletonLeanParts`: its first part swaying as a tree), turned into the plant's frame, `windLeanInPlant`); then its chains a level at a time, a thread a chain bone by bone (`windBoneOwnTurn`: hertz from metres, the plant's scale; seed `boneTreeSeed`, its pick's bits), barriers between levels; `GpuBonePose` (32 B) at first pose + 2 b, last frame's after it.
- **Shared buffers:** header (counts, dispatch), trees, plant table, pool (`WindBoneMotion::poolBones`, 262144 bones, 16 MB); each frame's passes wait for every earlier use. Tables remade (GPU idle first) when the skeletons, lean parts, pool or plants' room change.
- **F4** `BONES TREES n  STILL s  BONES asked/pool`. **`bones-compare`:** the frame's placed lists, trees and pool read back; `boneWantedPlants` against the claims (missing, extra, twice), each pose against `boneTreePoses` (the pass's order, equal to `windTreePoses`) from the leans the GPU wrote.
- **Cost grows with:** the claim with the swaying views' bound entries; the pose with the posed trees' bones, twice; memory the pool and 8 B a held plant.

## Debug view

`WindSettings::showField` (off): `drawWindField` (`engine/debug_views`) draws 9 x 9 arrows 4 m apart round the camera on the drawn ground (`terrainSurfaceHeight`), each half a second of wind, white to red towards 15 m/s.

## Settings

`WindSettings` (`RenderSettings::wind`, F5 Wind, `engine.render.wind.*`), live: `enabled`, cell sizes, `springs`, `flutters`, `showField`, `sway`, `swayFadeStart`/`End` (80, 400 m), `swayShadowCascades`, `swayLocalShadows`, `bones` (Bones, with its pool). Meanings and defaults: `render/gpu/wind.h`; `validateWind` lists what it rejects.

## Tests

`wind_test` (noise, velocity, springs, windows), `wind_sway_test`, `wind_hanging_test`, `sway_bones_test` (and `forest_trees_test`'s and `forest_test`'s bones), `wind_bones_test`, `bone_records_test`, `bone_poses_test`; GPU: `wind_gpu_test`, `wind_bones_gpu_test`.
