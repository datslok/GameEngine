# Grass and ground cover

Research for phase 18's grass and ground cover item: what covers a real jungle floor, how shipped games draw grass and small plants, and a plan for this engine. Written Nov 15 2026. Facts marked **(unverified)** come from secondary write-ups of talks behind GDC's paywall.

## What covers a rainforest floor

**Little grass under a closed canopy.** Seedlings at the forest floor get about 0.2-6.5% of full sun ([Montgomery 2004](https://repository.si.edu/bitstream/handle/10088/3713/Montgomery_2004.pdf)). Grass needs light, so it grows in the open: meadows, clearings, tree-fall gaps and path edges.

**Under the canopy:**

- **Tree seedlings** are the commonest small plants; herbs are fewer ([PMC6534666](https://pmc.ncbi.nlm.nih.gov/articles/PMC6534666)).
- **Ferns and Selaginella** (spikemoss) carpet damp, dim ground ([Botany One](https://www.botany.one/ecophysiology-of-early-lineage-vascular-plants/)).
- **Leaf litter** covers nearly everything, patchy over a few metres: its depth at one point says nothing about the next ([Barro Colorado study](https://typeset.io/papers/the-effect-of-leaf-litter-on-early-seedling-establishment-in-39qrqo5gm8)). Under palms it is five times deeper.

**For here:** grass only where `forestMeadowShare` says the canopy is open (the meadow on the rim, the clearing, the path's verges), and under the trees seedlings, small ferns, spikemoss and fallen leaves.

## How games draw it

### Ghost of Tsushima (Sucker Punch, GDC 2021)

The reference for grass. Talk: "Procedural Grass in Ghost of Tsushima", Eric Wohllaib ([GDC Vault](https://gdcvault.com/play/1027214/contactUs)). From write-ups **(unverified)**:

- **Tiles and compute:** the world is cut into tiles; a compute shader lays out each tile's blades from textures (height, grass type, density), culled by the frustum and distance, then drawn indirectly ([summary](https://gist.ly/youtube-summarizer/procedural-grass-systems-in-ghost-of-tsushima-achieving-art-direction), [Tiger Abrodi](https://tigerabrodi.blog/grass-in-ghost-of-tsushima)).
- **Blades:** a cubic Bezier curve each, 15 vertices near and 7 far, more of them towards the tip, where the bend is ([Unity-Grass](https://cainrademan.github.io/Unity-Grass/)).
- **Distance:** larger tiles drop 3 of 4 blades; past them, a texture.
- **Clumping:** blades grouped by Voronoi cells, each clump with its own height, lean and colour, which is what makes a field read as natural rather than uniform.
- **Short grass:** a blade folded in the middle reads as two.

### Horizon Zero Dawn (Guerrilla, GDC 2017)

All vegetation placed at run time on the GPU from density maps ([slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)):

- the world as 2D maps (about 4 MB a square km), each asset's density a function of them;
- 500+ asset types, about 250 in a frame;
- placement repeated as the player moves, so nothing is stored per plant.

### Unreal (Landscape Grass Type)

Grass instances spawned per landscape component from a layer's weight, with a density in instances per 10 m² and an end cull distance ([docs](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Landscape/FGrassVariety?lang=en-US)):

- Thinning by distance is not built in; users ask for it ([forum](https://forums.unrealengine.com/t/reduce-grass-density-by-distance/50086)).
- Whole components pop in at once; the advice is to fade the grass towards the ground's colour where it ends ([forum](https://forums.unrealengine.com/t/landscape-grass-type-grass-popping-in-based-on-landscape-components/1177750)).
- Wind (world position offset) can be switched off past a distance.

### What they share

| | Tsushima | Horizon | Unreal |
|---|---|---|---|
| Placed from | textures, per tile | density maps | a layer's weight |
| Stored per plant | nothing | nothing | instances per component |
| Far away | fewer blades, then a texture | cull | cull, fade to the ground's colour |
| Variety | clumps | rules per asset | a few meshes |

Nobody stores grass blade by blade over a whole map. The ground's own texture carries it in the distance, which is why it must match the grass's colour where the grass ends.

## What this means for this engine

**Memory rules out a mesh per patch.** The meadow (60 m round) and the clearing (120 m round) are about 56,000 m², 880 patches of 8 x 8 m. At 30 blades a square metre and 7 vertices a blade, each patch is about 0.8 MB with its coarser levels: about 1.2 GB, too much for the laptop's 8 GB. The asset manager is append-only, so patches cannot be streamed round the camera either.

**So a few shared patches, lifted onto the land in the vertex shader.** The terrain already does this: its patches are flat grids lifted onto the heights in `triangle.vert` (`terrain_data.glsl`). A ground cover patch is flat too (its blades standing on y = 0), drawn instanced at each 8 x 8 m cell; the vertex shader adds the land's height at the vertex's own place. Over a blade's few centimetres of lean, that height is the root's. A few variants and densities are a few MB in all.

**Density per cell, and the ground beyond.** Each cell picks a variant by its density (`forestMeadowShare` times the grass paint for grass; the rest for the forest floor's cover). The cells thin with distance through their levels of detail (all blades, half, a quarter, none), as Tsushima's tiles do, and the ground's grass texture carries on past them.

**Shadows:** none cast. Contact shadows and ambient occlusion ground it, as they do the understorey; a field of blades redrawn into the cascades would cost more than everything else on the floor.

**Wind** comes after, and these blades are its first users: a blade bends from its root by the square of the height up it.

## Plan

1. **Ground-following patches** (`ground-cover-patches`): the engine part. A mesh drawn lifted onto the terrain in the vertex shader (`ModelRenderer` or `Material` flag; the CPU's bounds raised by the cell's height range for culling), its CPU mirror tested against `terrain_heights`.
2. **Grass** (`meadow-grass`): blades as curves (5-7 segments, denser at the tip), clumped by Voronoi cells (height, lean, tone), in the meadow, the clearing and the path's verges, density from the open canopy and the grass paint; levels of detail to about 60 m; the ground paint's grass matched to the blades' colour where they end.
3. **The forest floor** (`forest-floor-cover`): fallen leaves lying on the ground, tree seedlings (the palms' and climbers' leaves on small stems), small ferns and spikemoss, under the canopy where there is no grass.

Each stage measured (triangles, the frame) before the next.
