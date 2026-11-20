# Level of detail: levels, impostors, card thinning

Covers: `src/scene/simplify_mesh`, `src/scene/impostor`, `src/scene/card_thinning`, `src/scene/card_thinning_shared.h`, `assets/shaders/impostor.glsl`, `assets/shaders/impostor_sampling.glsl`, `assets/shaders/card_thinning.glsl`, `assets/shaders/card_thinning_data.glsl`, `src/render/gpu/level_fade`, `assets/shaders/level_fade.glsl`

How a mesh's coarser stand-ins are made and named, far trees as cards, leaf cards fading out one by one, and levels cross-faded by a dither. How each view picks a level (`chooseLevel`, `chosenMesh`, `LevelView`, the settings): [game-interface.md](game-interface.md), Level of detail. Shadow tiles' own choice: [lights-and-shadow-atlas.md](lights-and-shadow-atlas.md). Research: `docs/research/LOD.md`. The forest's chains: [forest-trees.md](forest-trees.md), [forest-plants.md](forest-plants.md), [forest-rock.md](forest-rock.md).

## A mesh's levels

- `MeshLevel` (`scene/mesh_level.h`): {mesh, `errorMetres` (how far it strays from the mesh), `sharesVertices`}. Coarsest last; a last level without a mesh draws nothing past its error.
- **Naming:** `AssetManager::setMeshLevels(base, levels)`. Throws for more than three (`meshLevelSlots` 4 counts the mesh), a level that is the base or not yet added, a missing mesh before the last, or errors not rising from 0. Bumps `getLevelsVersion`, which rebuilds kept still draws.
- **Options:** `setMeshLevelOptions(mesh, MeshLevelOptions)`: `hiddenBelowError` (an impostor card, drawn only where that error fits) and `chooseBounds` (one box shared by a tree's parts and its card, so all switch at one place). Throws for an unknown mesh or a negative error.
- **Generated levels** (`scene/simplify_mesh`, meshoptimizer):

| Function | Does |
|---|---|
| `simplifyMesh(mesh, keepShare, maxError, lockBorder, &errorMetres)` | Collapses edges cheapest first until `keepShare` of the triangles are left or the next would move the surface past `maxError` (a share of the mesh's size); `lockBorder` keeps open edges (a cut-out leaf's outline); unused vertices dropped; `errorMetres` = meshoptimizer's error times the mesh's size |
| `simplifiedLevels(mesh, keepShares, maxError)` | One `SimplifiedLevel` {mesh, `errorMetres`} a share, each from the mesh itself, borders free, errors forced rising |
| `AssetManager::addDrawnMeshWithLevels(mesh, levels)` | Adds a drawn-only mesh and its levels, named on it |

- **Prefix levels** (`scene/indexed_mesh`): `meshPrefix(mesh, vertices, indices)` is a level made of the mesh's first vertices and indices (throws for a triangle reaching past them); `thinnedLevel(mesh)` cuts a card-thinned mesh to its kept cards. Such a level sets `sharesVertices`, so the mesh's per-vertex records still apply to it.
- **Per-vertex records follow the draw's own mesh** (`attachVertexEntries` by `DrawItem::mesh`), so a mesh with hanging entries ([wind.md](wind.md)) must have no coarser level that does not share its vertices.

## Impostors

A far tree as one card facing the viewer, showing pictures baked from many directions; Brucks 2018 and Epic's Impostor Baker (`docs/research/LOD.md`, Impostors). Maths in `scene/impostor`, mirrored in `impostor.glsl`.

**Directions.** `frames` x `frames` over the upper hemisphere folded onto a square (`hemiOctahedralEncode`/`Decode`: straight up the middle, horizon the edge, below it counts as on it). `impostorFrameDirection(frame, frames)`: frame (column, row) corner to corner over the square. `impostorFrameBasis`: right level with the ground, up square to it and the direction.

**The bake** (`bakeImpostor(parts, frames, frameTexels, jobs)`, on the CPU at start-up, a row of frames a job):

- `ImpostorPart`: a mesh in the tree's frame, its colour texture (or none: tint alone), sRGB tint, cut-out cutoff (0 solid).
- Each frame: the parts drawn orthographically along its direction over the sphere round them; nearest surface kept; holes skipped; a double-sided leaf's normal turned to the viewer.
- Out (`ImpostorBake`): a colour atlas (sRGB, alpha 255 where something was) and a normal atlas (the tree's frame, n · 0.5 + 0.5), plus the sphere's centre and radius. Empty texels take their neighbours' colour (alpha 0), so mips do not darken the edges.
- Leaves are lit from the front only in the bake (`shading.md`).

**The card** (`impostorCardMesh(centre, radius)`): two triangles, corners a radius round the middle in x and y (so bounds hold the sphere), UVs the corners (-1..1), the normal's x the radius.

**Material** (`impostorMaterial(colour, normal, frames)`): a double-sided cut-out of the colour atlas (`impostorCoverageCutoff` 0.5, coverage-keeping mips), the normal atlas in the normal map's slot, `Material::impostorFrames` set. Packed as the `MATERIAL_IMPOSTOR` flag and the frames in `GpuMaterial::attenuation.w`.

**Drawing:**

- `packInstance` marks any draw whose material has `impostorFrames` as `INSTANCE_KIND_IMPOSTOR` (`GpuInstance::material[3]`).
- `triangle.vert` turns the card to face the viewer (a capture's own eye) on the frame plane's axes for that view direction (`impostorFrameRight`, `impostorFrameUp`), passing the card point and view direction in the tree's frame.
- `sampleImpostor` (`impostor_sampling.glsl`; from `scene_surface.glsl` and `cut_out_prepass.frag` for the holes): the four frames round the view direction blended bilinearly (`impostorFrameBlend` on the CPU), each read where the card point falls on its own plane (`impostorFrameUv`, `impostorAtlasUv`), mips from the card point's screen steps carried onto each plane. The blended normal, turned into the world by the tree's axes, is lit, so the sun still moves over the card.
- No ray meets a card (`rayMaskFor`); games give cards no shadow (stand-ins keep casting). Cards do not sway.

**Switching** (all through the level chain):

- The tree's parts end on a level without a mesh at the card's error; the card has `hiddenBelowError` at that error; all choose by the tree's `chooseBounds`, so exactly one of tree and card is drawn.
- Levels off: the tree whole, never the card. `LevelOfDetailSettings::impostors` off: a level without a mesh is never chosen, so the tree keeps its last real level however far (for comparing).

## Card thinning

A mesh of cards (leaf clusters, palm leaflets) fades into its coarser level one card at a time instead of switching: dropped cards shrink away, kept ones grow (SpeedTree's runtime; Cook et al. 2007; `docs/research/LOD.md`, Hiding the switches). Maths in `scene/card_thinning`, mirrored in `card_thinning.glsl`.

**Per mesh** (`IndexedMesh::thinning`, a `CardThinning`): `cardSize` (metres; 0 = not thinned), `growth` (kept cards' sides grow by share^-growth; 0.5 keeps their area), `leastShare` (what the coarser level keeps). **Per vertex** (`IndexedMesh::thinningCards`, a `ThinningCard` each): its card's anchor (the stem it shrinks and grows about) and rank (0: always kept).

| Function | Gives |
|---|---|
| `thinningError(thinning, share)` | cardSize (share^-growth - 1): how far the cloud strays |
| `thinnedShare(thinning, error)` | Its inverse, held to [leastShare, 1]; 1 when not thinned |
| `cardPresence(rank, share)` | 1 at or above the rank, 0 a `CARD_THINNING_BAND` (0.05) below, smooth between, so about that share of cards are shrinking at once and none pops |
| `thinnedCardScale(thinning, rank, share)` | share^-growth times presence |
| `thinnedPosition(position, card, scale)` | The corner scaled about its anchor |
| `removedCardRank(random, leastShare)` | A rank in [leastShare + band, 1] for a card the coarse level drops |

**Matching the level switch:**

- The coarser level is the kept cards alone, first in the mesh (`thinnedLevel`, `sharesVertices`), so it is drawn with the same records, grown as the fade left them: the switch changes nothing seen.
- Each view allows `thinningErrorPerMetre` = pixelError / pixelsPerMetre metres of error a metre away; at the draw's level distance (from `GpuInstance::levelSphere`, the level bounds or the bounds) the share is `thinnedShare` of that error. It reaches `leastShare` exactly where `chooseLevel` takes the level whose error is that share's.
- Orthographic views and views without levels allow 0: nothing thins.

**On the GPU:**

- `uploadMesh` packs a thinned mesh's header (card size, growth, least share) and a record a vertex (`packCardThinning`, `GpuThinningRecord`, 16 bytes) into scene set 1 binding 42, copied whole when new ones arrive (`updateVertexRecords`), and returns the entry in `VertexEntries`; `attachVertexEntries` sets `DrawItem::thinningEntry`, packed into `GpuInstance::hanging[2]`.
- `batchByMesh` marks a draw drawn with the mesh itself or a vertex-sharing level of a thinned mesh (`BatchedDraws::marks`); `appendDrawList` sets `DRAW_LIST_THINNED_BIT` in its draw list entry (`drawnInstance` strips it).
- `thinnedVertex` (`card_thinning_data.glsl`) shapes marked instances' vertices in `triangle.vert` (allowance `GpuFrameData::levelOptions.x` for the camera, `GpuCaptureView::eye.w` for a probe face or the mirror) and `triangle_motion.vert` (the main pass, last frame's position too).
- Shadow tiles never mark entries: a thinned mesh must cast no shadow (its shadow stand-in does).

## Level fades

A mesh whose levels and card would swap in one frame (the scanned plants' simplified meshes, which have no cards to thin) instead draws both over a band of distance before each switch, each level keeping the pixels the other throws away (Unity's LODGroup, Unreal's dithered transitions; `docs/research/LOD.md`, Hiding the switches). Distance-based: nothing is kept from frame to frame.

- **Which:** `MeshLevelOptions::fadesLevels` on the mesh and its card (`DrawItem::fadesLevels`, `CULL_FADES`). Only cut-out materials: the depth prepass has no fragment shader for solid ones.
- **Where:** `LevelOfDetailSettings::fadeBand` (0.2; 0 to `fadeBandMost` 0.9, 0 swaps), in the camera's view only (`cameraLevelView`); captures, the mirror and shadow tiles (`levelView`, orthographic) never fade.
- **The band** (`levelFade`): the levels chosen under the budget (the finer) and under pixelError / (1 - band) (the coarser: the draw as if 1 - band times as far). Where they differ both are drawn; either may be none (a plant fading into its card has no coarser mesh, its card no finer one, by the same error and box). Share = (pixelError / pixels - (1 - band)) / band, pixels the coarser's error on screen: 0 at the band's start, 1 at the switch.
- **The field** (`level_fade`, mirrored in `level_fade.glsl`): `levelFadeField(share, coarse)`, 7 bits of the draw list entry above its 24-bit instance (`DRAW_LIST_FADE_*`; `drawListInstances` 16.7 M, past which `CameraCullTable::update` throws): the coarse bit and the share's step plus 1 (63 steps; 0 no fade). `levelFadeThreshold` is the step's middle; `levelFadeKeeps(field, noise)` keeps the coarser below it, the finer from it, so the two split every pixel.
- **The noise** (`levelFadeNoise`): interleaved gradient noise of the pixel, offset from the cascades' dither and turned by 5.588238 x the frame (`GpuFrameData::levelOptions.y`, 0 without temporal anti-aliasing), which TAA and the upscalers average into a blend.
- **Drawing:** `triangle.vert` passes `drawListFade(entry)` on (location 11); `cut_out_prepass.frag` discards what its level does not keep, after its texture reads; the main pass shades, and writes motion for, only what the prepass kept (EQUAL). `batchByMesh` (the CPU's reference) lists a fading draw twice, its marks carrying the fields; `levelsChosen` counts both levels.
- **Cost:** a draw in a band is drawn twice there, at its cheaper levels; grows with the fading draws in view.

## Tests

`simplify_mesh_test`, `impostor_test`, `card_thinning_test`, `level_fade_test`; GPU: `impostor_gpu_test`, `card_thinning_gpu_test`, `level_fade_gpu_test`. Choosing levels and fades: `level_of_detail_test` ([game-interface.md](game-interface.md)).
