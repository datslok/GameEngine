# Wind bones

Covers: `src/render/gpu/wind_bones`, `src/render/gpu/wind_bones_shared.h`, `assets/shaders/wind_bones.glsl`, `assets/shaders/quat.glsl`, `src/render/gpu/bone_records`, `src/render/gpu/bone_records_shared.h`, `src/render/gpu/bone_poses`, `src/render/gpu/bone_poses_shared.h`, `assets/shaders/bone_poses.glsl`, `assets/shaders/bone_pass.glsl`, `assets/shaders/bones_claim.comp`, `assets/shaders/bones_pose.comp`, `src/render/vulkan/vulkan_bone_posing`, `src/scene/sway_bones`, `assets/shaders/bone_vertex_data.glsl`

Trees move by bones instead of the per-vertex sway ([wind.md](wind.md), `windSwayOffset`): each frame a pass poses every swaying tree's skeleton on the GPU from the trees' spring at its foot, and the draws move each bound vertex by its two bones' poses. Research and plan: `docs/research/LEAFGEOMETRY.md`, Wind with valid bounds (phase 21 stage 3c). Built: the bones and bindings (3c-i), their motion's maths (3c-ii), every tree mesh bound and the records on the GPU (3c-iii-a), the bones pass (3c-iii-b), bound vertices and twigs drawn by the poses (3c-iii-c-i, ii); lianas' anchors still move by the sway (3c-iii-c-iii).

## Bones and bindings

- `SwayBone` (`scene/sway_bones`): pivot, tip, parent (before it in the list, -1 a root), level (0 the trunk, +1 a side chain), the line's radius at its pivot; its chain's first bone, length and radius at its start (`chainStart`, `chainLength`, `chainRadius`).
- `SwayBinding`: a vertex's bone and its weight, the rest its parent's (two-bone blending, GPU Gems 3 ch. 6), so joints never open.
- `appendSwayChain`: a line cut into whole bones of one length near `boneMetres`, each the one before's child; `swayArcOfPoint` (how far along a line a point lies); `swayChainBinding` (the bone at an arc, weight rising from 0 at its pivot over its first `blendShare`, a root's whole); `swayBonesOrdered`.
- Trees: `treeBones` (`games/forest/forest_trees`) cuts each chain, chains taken by first node so parents come first, a side chain's first bone the child of the bone holding its start node; `TreeMeshes::bones`; `bark.swayBindings` (each bark vertex by its own chain's arc), its levels by `simplifyMesh`; the cards whole, each by its node (`appendClusterCards` given the nodes' bindings, `nodeOrNearest`), as `PlacedTwig::binding` (its node's; a filler its nearest node's); climbers and epiphytes by the nearest bark (`swayLines`, `bindToTreeBones`, `swayNearestBinding`). `ForestTreeSettings::boneMetres` 2 m, `boneBlendShare` 0.5 (F5 Forest, at start-up).

## Motion

(`render/gpu/wind_bones` and `wind_bones.glsl`, mirrored; quaternions in `quat.glsl`, mirroring `math/quat.h`); all in one frame, metres, the tree's lean given in it:

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

## The records on the GPU

(`render/gpu/bone_records`, 3c-iii-a)

- A part moved by bones carries its look's `SwaySkeleton` (bones, height in mesh units): `RenderPart::skeleton`, `DrawItem::skeleton`; the forest gives it every swaying tree part (bark, leaves, shadow stand-ins, twigs, climbers, epiphytes), not the impostor card. The placed table lists them once (`PlacedTable::skeletons`), a part's `GpuPlacedPart::unused[1]` its number plus 1.
- `packSwaySkeletons`: `GpuSwayBone` (48 B; parent and chain start within its skeleton), `GpuSwaySkeleton` (32 B: run, height, foot radius, its chain levels, its lean part), `GpuSwayChain` (first bone, count) by level (trunk, its branches, theirs) with each level's start; refuses a chain not a run of children of one level, or starting on a bone not of a lower level.
- Vertex bindings beside the vertex pool, a word a pool vertex (`packSwayBinding`: bone low 16 bits, weight unorm16 high; `SWAY_UNBOUND` otherwise), read by the vertex's place in the pool, so levels with their own vertices and cluster levels need no per-mesh entry; `placeSwayBindings` at upload, the list ending at the last bound mesh (scene binding 48). 4 B a pooled vertex beside its 48 B.
- Twigs: `TwigInstance::binding`, `packTwigBinding`, a word a twig beside the twigs (`VulkanTwigDrawing::getTwigBindings`, scene binding 49).

## The bones pass

(3c-iii-b; `render/gpu/bone_poses` mirroring `bone_poses.glsl`, `render/vulkan/vulkan_bone_posing`, `bones_claim.comp`, `bones_pose.comp`, GPU timer BONES): after [PLACED] and [WIND] (moved there), before the twigs' and cluster passes, while things sway and placed parts carry skeletons.

- **Swaying views:** the camera (placed view 0) and the shadow tiles drawn swaying (`tileSways`); probe captures draw trees still.
- **Claim** (a workgroup a group of a bound part (`boneGroups`) and swaying view, a thread an entry): a plant nearer than `swayFadeEnd` (`boneTreeWanted`) claimed by an `atomicExchange` of the frame's stamp in its word pair (stamp, tree; 8 B a held plant, never cleared after it is made); the claimer takes the next tree (`GpuBoneTree` 32 B, up to `BONE_MOST_TREES` 65536) and its bones' room (`boneTreeFirstPose`: two poses a bone from twice the bones asked before, or `BONE_NO_POSE`, standing still), raising the pose pass's dispatch.
- **Pose** (indirect, a workgroup a tree): its lean now and last frame (`windFadedLean` of the trees' spring at its foot by its skeleton's lean part's bend (`skeletonLeanParts`: its first part swaying as a tree), turned into the plant's frame, `windLeanInPlant`); then its chains a level at a time, a thread a chain bone by bone (`windBoneOwnTurn`: hertz from metres, the plant's scale; seed `boneTreeSeed`, its pick's bits), barriers between levels; `GpuBonePose` (32 B: turn, shift, parent) at first pose + 2 b, last frame's after it.
- **Shared buffers:** header (counts, dispatch), trees, plant table, pool (`WindBoneMotion::poolBones`, 262144 bones, 16 MB, at first; a frame asking for more grows it to the next power of two above the ask, under `poolMostBones`, 4M bones: `boneGrownPool`, read back at the frame's fence, made at the next prepare; a setting changed starts it again); each frame's passes wait for every earlier use. Tables remade (GPU idle first) when the skeletons, lean parts, pool or plants' room change.
- **F4** `BONES TREES n  STILL s  BONES asked/pool`. **`bones-compare`:** the frame's placed lists, trees and pool read back; `boneWantedPlants` against the claims (missing, extra, twice), each pose against `boneTreePoses` (the pass's order, equal to `windTreePoses`) from the leans the GPU wrote.
- **Cost grows with:** the claim with the swaying views' bound entries; the pose with the posed trees' bones, twice; memory the pool and 8 B a held plant.

## Drawn by the poses (3c-iii-c-i, ii)

`bone_vertex_data.glsl` (scene set 1: trees 50, each plant's tree 51, the pool 52, the vertices' bindings 48), read by `triangle_vertex.glsl` (the camera's vertex and mesh shaders) and `shadow.vert`, `shadow_cut_out.vert` (tiles that sway):

- **Which vertex:** a placed plant's (`DrawnObject::plant`) pool vertex (`gl_VertexIndex`, a mesh shader's vertex number) bound to a bone, or a twig's (bound by the twig's own binding, scene binding 49).
- **Its tree** (`boneDrawn`): `boneClaimedTree` of the plant's word pair against the frame's stamp in the wind record (`GpuWindField::flags[1]`, `VulkanBonePosing::getNextStamp`, `boneNextStamp`; 0 on a frame the pass does not run, matching nothing), then the tree's first pose and leans.
- **Its poses** (`boneVertexPoses`, mirrored in `render/gpu/bone_poses`): its bone's at `bonePoseSlot` (last frame's after it) and its parent's, named in the pose's fourth word (`GpuBonePose::parent`, -1 a root: the rest pose), so draws need no bone table.
- **Moved** (`boneSwayOfVertex`): the blended place minus the rest, in the mesh's frame, then turned and scaled by the model (bones live in the plant's frame: a bound part sits at its plant's origin, `buildPlacedTable` refuses another); leaves flutter as the sway's (`windFlutterAmount` by the tree's lean); a climber's own anchors move as its bones, its swing as before (`windHangingSway`). Normals and tangents turned by `windBlendedDirection`. The main pass's last place from last frame's poses.
- **Twigs** (ii): each moves whole, by `windRigidPose` at its foot (the twig matrix's place): its two bones' turns blended the short way (nlerp), its foot blended as the bark there is, so it stays on its branch. A mesh shader's workgroup draws one twig's cluster: its first thread makes the pose (`twigPoseOf`, last frame's too in the main pass) and shares it, every vertex then posed by it as by one bone (`drawnPoses`); leaves flutter as the cards' do. Swaying twigs skip the per-twig frustum test (`meshClusterRigid`), so they cannot pop at the screen's edge until 3c-iv's posed bounds.
- **Still:** a bound vertex whose tree is not posed (past the reach, unclaimed, no room, a tile drawn still, a capture) keeps its rest shape; no fallback to the sway.
- **Cost a bound vertex:** a binding word, the plant's pair, a tree record and two poses (four in the main pass); grows with drawn bound vertices. A twig pays that once a cluster, then a pose a vertex.

## Tests

`sway_bones_test` (and `forest_trees_test`'s and `forest_test`'s bones), `wind_bones_test`, `bone_records_test`, `bone_poses_test` (claims, poses, the draws' reads), `wind_bones_test`'s rigid pose, `placed_drawing_test` (a bound part at its plant's origin), `wind_sway_test` (`windFlutterAmount`); GPU: `wind_bones_gpu_test`, `wind_gpu_test`.
