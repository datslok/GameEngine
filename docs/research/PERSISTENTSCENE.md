# A persistent scene

Research for ROADMAP phase 18's "persistent scene" item: how shipped engines stop paying the CPU for objects that never move, what this engine pays today, and a staged plan. Written Nov 15 2026.

## The problem in numbers

The forest among the trees (the RTX 4070 Laptop, after `frame-copies`), about 35,000 draws, standing still:

| Part | ms | What it does for every draw, every frame |
|---|---|---|
| BUILD | 6.0 | Interpolated matrix, a 520-byte `DrawItem` built and copied (its 280-byte `Material` too), last frame's matrix read and this one's remembered |
| ATTACH | 2.5 | Bounding sphere (four square roots), flat faces, levels of detail; the sky bake's fingerprint hashes every static draw |
| PACK | 9.3 | 272 bytes of material packed and hashed byte by byte, deduplicated from scratch; normal matrix inverted; 7.8 MB written to upload memory |
| CAMERA | 0.9 | Frustum test of every draw, level chosen twice, sorted into batches |
| SHADOWS | 1.7 | A full scan per shadow reach, and every caster's signature hashed even when the tile is cached |

The frame is 24.3 ms, the GPU's 21.5 ms. Nothing is kept between frames: every byte of the GPU scene is rewritten every frame, and nothing on this path reads `ModelRenderer::isStatic`.

**The forest marks almost nothing static.** Trees, leaves, plants, rocks, moss, the Ring and the far towers all pass `isStatic = false`, though none of them move; only the ground and the Monolith are static. `isStatic` means "part of the level the light probes and the sky bake treat as solid", which the forest keeps the canopy out of on purpose. So "never moves" needs a flag of its own.

**Materials change without a flag:** the forest's `updateDaylight` rewrites the ground's, the terrain's and every rock's material each frame.

## How others do it

### Unreal Engine: the GPU Scene

- Each primitive gets a **persistent index**, a slot it keeps from when it is added until it is removed (`FPersistentPrimitiveIndex`). Its data lives at that slot in scene-wide GPU buffers, and shaders read it by id.
- **Only dirty primitives are uploaded.** A dirty list and a per-slot dirty state; `r.GPUScene.UploadEveryFrame` exists only for debugging. Changed records go up through a scatter upload: packed together with their target slots, and a small compute shader writes each into place.
- **Static primitives cache their draw commands once**, when added to the scene; each frame only selects the cached commands per view. Dynamic ones rebuild theirs every frame. Invalidating every cache at once "will cause a hitch in larger scenes" (Epic's Mesh Drawing Pipeline docs, UE 4.22).
- Removed slots go back to a free list; the buffers are not compacted (from memory of the source, unverified).
- An instance with no previous transform uses its current one, so a still object needs no write to have zero motion.
- Nanite: "renderer now retained mode", the scene sparsely updated where things change, culled on the GPU against last frame's depth pyramid (Karis, SIGGRAPH 2021).
- Known failure: hitches when many primitives are dirtied in one frame (Epic forums).

### Unity: BatchRendererGroup and the GPU Resident Drawer

- **BatchRendererGroup:** instance data lives in a persistent GPU buffer (112 bytes an instance in Unity's sample). Each frame the CPU writes only a small visibility list of instance numbers, which the shader uses to find the real record. Changes go to a CPU copy, uploaded in one to four calls.
- **GPU Resident Drawer** (Unity 6, built on it): conversion "only executes when things change". Culling still runs on the CPU. Unity's measured result: "some larger scenes ... halving the CPU frame time", small scenes marginal. Its docs warn that it "slightly increases GPU workload", so a GPU-bound game can get slower.

### GPU-driven pipelines

- **Assassin's Creed Unity** (Haar & Aaltonen, SIGGRAPH 2015): per-instance data in a GPU ring buffer, "persistent for static instances"; the GPU culls instances and clusters and draws by indirect calls. The CPU cost was about 75% of earlier games with about ten times the objects.
- **RedLynx's benchmark** (same talk): 250,000 moving objects in two indirect draws, the level of detail chosen on the GPU in the culling pass, **0.2 ms of CPU on one Xbox One core**. Two-phase occlusion: cull against last frame's depth pyramid, rebuild it, retest what was rejected.
- **Frostbite** (Wihlidal, GDC 2016): triangle and cluster culling in compute; "pre-build, cache and reuse, or generate on the GPU". About GPU triangle work, not CPU cost.
- **HypeHype** (Aaltonen, REAC 2023): 10,000 draws issued every frame with no persistent data took 0.85 ms on one laptop core. Its rule: "Persistent data: upload once at startup, delta update when data changes."
- **Bevy** (open source, the same design as Unreal's): only entities changed since last frame are looked at; a removed entry is marked free; each record keeps the slot it had last frame, for motion vectors.

### The common pattern

1. **A stable slot per object**, from a free list, never moved.
2. **A dirty list:** a change pushes the slot once (a bit per slot stops duplicates).
3. **A scatter upload:** dirty records packed with their slots and copied into one persistent GPU buffer. Vulkan's `vkCmdCopyBuffer` with many regions does it with no shader. Cost goes with the changes, not the scene.
4. **Previous transforms:** a still object keeps previous = current forever. A mover that stops must be written once more, or its motion vectors stay stale (a bug reported with Unreal's pooled meshes).
5. **Culling and level of detail on the GPU** come later, and are what removes the per-view scans.

Published CPU savings at tens of thousands of objects on PC are few: Unity's "halved", AC Unity's 75% for ten times the objects, RedLynx's 0.2 ms. Our 17.8 ms for 35,000 objects is far above HypeHype's 0.85 ms for 10,000 fully redone draws, so much of it is overhead per object (copies, hashing byte by byte, lookups), not bandwidth.

## What it would buy here

**The GPU is the limit next.** Frames are the slower of the CPU and the GPU. The CPU is at 24.3 ms and the GPU at 21.5 ms, so even a CPU frame of 10 ms only takes the frame to about 21.5 ms: about 2.8 ms, or 41 to 46 fps. What it really buys is headroom: room for wind, vines, more plants and the simulation, all without the CPU becoming the limit again. The GPU's own costs (the main pass 9 ms, anti-aliasing 3.7, the reflection probe 3.1) are a separate job.

## The plan in stages

Each stage is its own branch, measured before and after. All of it is render-only, so the simulation's determinism is untouched.

### Stage 1: still objects (`still-objects`)

A flag for "never moves", and the engine keeping still objects' draws between frames.

- **`ModelRenderer::still`** (off by default): the object's transform and parts never change. Separate from `isStatic`, which keeps its meaning. A still object needs no `PreviousTransform`; its last frame's matrix is its current one.
- **Changes are announced:** a game that changes a still object (a material, a part, a move) calls `markRenderChanged(world, entity)`, and its draws are rebuilt that frame. A still object that moves without it is a game bug; a debug-build check compares a sample of still matrices each frame and throws.
- **The engine keeps the still draws** in a list of their own, built when an entity is added or changed: matrix, bounds, flat faces and levels attached once. Each frame only the moving draws are built and attached, then appended.
- **The forest** marks its trees, plants, rocks, moss, hangings, the Ring and the towers still. Its daylight material updates announce themselves, or move to a uniform the materials read.
- **Expected:** BUILD and ATTACH from 8.5 ms to well under 1 ms. The sky bake's fingerprint walks only what changed.

### Stage 2: a persistent GPU scene (`gpu-scene-persistent`)

- **Instances in slots:** each draw gets a slot in one GPU buffer that lasts across frames. Still draws are written once; moving draws every frame. Dirty records go up by one `vkCmdCopyBuffer` with a region each (adjacent slots merged), ordered on the queue so frames in flight never see a half-written buffer.
- **Materials in a persistent table:** deduplicated when a draw is added, with reference counts. The byte-by-byte hash goes (hash words, or key materials by their source).
- **Last frame's matrices:** still draws read their current one; only movers write a previous matrix.
- **Removal:** a slot goes back to a free list, its record marked empty so no view draws it.
- **Expected:** PACK from 9.3 ms to the movers' share, well under 1 ms.

### Stage 3: views without full scans (later, measured first)

What is left: CAMERA (0.9 ms) and SHADOWS (1.7 ms) scan every draw for every view. Two ways, chosen by measuring after stage 2:

- **On the CPU:** a grid of still objects' bounds, so each view tests cells, not objects; shadow signatures from a still tile's version rather than hashing every caster.
- **On the GPU:** culling and levels of detail in a compute pass writing indirect draws (RedLynx's design). The bigger change: the shadow cache, glass sorting and the planar mirror's choice all read the CPU's lists today.

### Stage 4: changes announced, not found (`still-announced`)

After GPU-driven drawing, the CPU still walked every renderer twice a frame (`StillDraws::update` for additions and removals, `buildFrame` for the moving ones): about 0.7 ms in the forest, growing with the world. How others learn of changes (from the engines' documentation and source, from memory):

| Engine | How |
|---|---|
| Unreal | The game calls `AddPrimitive`, `RemovePrimitive`, `UpdatePrimitiveTransform`; the scene keeps dirty lists and never scans |
| EnTT, flecs | Signals or observers when a component is added or removed (`on_construct`, `on_destroy`; `OnAdd`, `OnRemove`) |
| Bevy | Component hooks and observers keep its retained render world in step (0.14, 0.15) |
| Unity Entities | A change version per chunk of about 128 entities, bumped on write access: every chunk still checked, 128 times fewer checks |

Chosen: Unreal's and EnTT's, since Unity's still grows with the world. The world logs entities gaining or losing a `ModelRenderer`, `Transform` or `DeformingMesh` (`World::logChanges`); shown and still are private, changed only by setters that announce (`setRenderVisible`, `setRenderStill`; the user chose them over plain fields with a debug check, since the check samples and misses more as the world grows); the still draws keep the moving list themselves. Cost: the changes and the moving entities.

## Risks

- **A still object that moves unannounced** draws in the wrong place. Hence the debug check, and still being off by default.
- **Many changes at once** (a level loading, every material changing) cost as much as today for that frame, as Unreal's hitches show. Acceptable: no worse than now.
- **Two sources of truth** (the world and the kept draws) are the main thing to test: a frame built from kept draws must equal one built from scratch, draw for draw.
