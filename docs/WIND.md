# Wind

Research for phase 18's wind item: how shipped games move foliage in the wind, what real wind and plants do, and a plan for this engine. Written Nov 16 2026. Facts marked **(unverified)** come from secondary sources or forum posts.

## Real wind and plants

**Wind under a rainforest canopy is weak.** In the Amazon, from the floor up to about 0.65 of the canopy's height the wind is nearly constant with height, under 1 m/s, and rises above it up to about 2.25 times the canopy's height ([Advances in Meteorology 2017](https://doaj.org/article/377c6022db1e477892bd5040c255e4a5)). Turbulence falls off sharply in the canopy's top third; the fast downward sweeps of gusts do not reach the lower canopy ([turbulence study](https://link.springer.com/article/10.1023/A:1002401829007)). Above the canopy, a few m/s is typical **(unverified)**.

**So:** the crowns sway, the understorey barely stirs, and the grass in the open (the meadow, the clearing) moves most near the ground.

**Trees sway slowly.**

- Maples measured 0.4-0.56 Hz, a sway of about 2 s ([Dendrobiology](https://www.idpan.poznan.pl/images/stories/dendrobiology/vol64/64_37_42.pdf)); taller trees are slower.
- A cantilever beam predicts a conifer's frequency well, not a broadleaf's: its branches damp and detune it ([J R Soc Interface 2019](https://pubmed.ncbi.nlm.nih.gov/31164076/)).
- Sway follows the wind averaged over a minute or more, not second by second: a trunk filters gusts out.

**Hanging things swing as chains:** a period of 5.22 sqrt(L / g), 7.5 s for a 20 m liana, 3.7 s for 5 m (`docs/VINES.md`).

## How games do it

### Horizon Zero Dawn (Guerrilla, GDC 2018): the one chosen

The user's pick (Nov 16 2026). From the slides themselves, "Between Tech and Art: The Vegetation of Horizon Zero Dawn", Gilbert Sanders ([PDF](https://media.gdcvault.com/gdc2018/presentations/gilbert_sanders_between_tech_and.pdf), slides 13-30 and 83):

**One global wind force field drives all movement.**

- A compute shader, about 150 microseconds a frame, local round the player and camera. Its debug view shows near, mid and far cascades, turbulence and flow, probes, and a solver per category.
- Physical properties baked in: **springs**, in up to four categories with their own settings: trees and plants (the vegetation shader), grasses (the grass shader), and "special", sampled by banners, tarps and canvas.
- So the field holds not the wind itself but how each kind of plant has answered it: a spring pushed by the wind, lagging, overshooting and settling as real stems do.

**Trees: sampled once, at the object's centre.** The sampled force:

- bends the tree, by its **height up the object** (its rigidity);
- bends the branches, by their **distance from the trunk** (bend, sway and lift);
- its length drives a tiny 16 x 16 x 16 3D simplex noise texture moving the leaves, by their **distance from the branch** (amount).
- Per-vertex data holds the three distances, an index or offset, and baked ambient occlusion.

**Plants:** most have the tree's bend removed; a soft clamp keeps big forces from flattening them: f(x) = x (a + 1) / (x + a).

**Grass:** the global wind plus ambient motion:

- large scale, phased by the object's centre c, so a tuft moves as one: x = 2 sin(c.x + c.y + c.z + t) + 1, y = sin(2 (c.x + c.y + c.z + t)) + 0.5;
- small scale, phased by the vertex's place p, along the normal: 0.065 sin(2.65 (p.x + p.y + p.z + t)) times (1, 1, 0.35);
- tilted towards the camera by its height, so cards never show an edge; hugging the ground from the height map;
- over distance the animation scales down and the vertices are pushed down, so far grass neither shimmers nor pops.

**Shadows:** separate shadow meshes. The nearest cascade (to 8 m) casts the third visual level, alpha-tested and **animated**; the next (to 80 m) casts fewer triangles, **not animated**.

### Crysis (GPU Gems 3, chapter 16)

Two layers, both in the vertex shader ([chapter](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)):

- **Main bending:** the whole plant bends along the wind, more the higher up it is.
- **Detail bending:** leaves flutter, their phase and stiffness per vertex (artists painted them).
- Triangle waves instead of sines: cheaper, and a sum of a few looks irregular enough.

### SpeedTree (Unreal, Unity)

Layers from slow to fast ([SpeedTree's manual](https://docs.unity3d.com/speedtree-modeler/manual/wind-games.html)):

| Layer | What moves | Speed |
|---|---|---|
| Global sway | the whole tree, the trunk only this | slowest |
| Branch motion | each big branch bends with the wind and oscillates | faster |
| Leaves | each leaf on its own, "waves rolling through" when neighbours share phase | faster still |
| Ripple, flutter | leaves along their normals, random turns | fastest |

- **Gusts** roll through the tree with a speed and a width; how often they come is a setting.
- **Quality tiers:** the lowest applies global sway only.
- The rule: each layer slower than the one below it.

### Unreal's Pivot Painter

Each branch's and leaf's pivot, parent and direction baked into textures; the shader rebuilds the hierarchy and turns each part about its pivot, inheriting its parent's motion ([Epic's docs](https://dev.epicgames.com/documentation/unreal-engine/pivot-painter-tool-2.0-in-unreal-engine)). The most faithful, and the most data.

### Ghost of Tsushima (GDC 2021)

"Blowing from the West" ([GDC Vault](https://gdcvault.com/play/1027350/Blowing-from-the-West-Simulating)): one wind system for grass, trees, particles and the guiding wind; "volume over accuracy", heuristics rather than simulation ([write-up](https://gamedeveloper.com/design/using-vorticles-to-simulate-wind-in-i-ghost-of-tsushima-i-)). Its grass bends each blade on its own **(unverified)**.

### The two traps

- **Motion vectors.** Foliage moved in the vertex shader without its own motion smears under temporal anti-aliasing: the history is reprojected as if it stood still. Unreal's fix is writing velocity from the wind's motion, which needs last frame's displacement too ([forum](https://forums.unrealengine.com/t/foliage-wpo-velocity-for-taa-smearing/148166/12), **(unverified)**).
- **Cached shadows.** Anything that moves every frame redraws the shadow tiles it is in every frame. This engine caches its sun cascades and redraws them only when something changes; the leaves alone once cost 23 ms of shadows on the laptop when the camera turned. Unreal's virtual shadow maps have the same problem with foliage (`docs/SHADOWCACHE.md`).

## What this means for this engine

**Horizon's design fits it.** A field of springs round the camera, computed on the GPU each frame like the fog's grid, which every vertex shader samples. The springs give the lag and overshoot that make gusts read as wind rather than as a looping animation. The same field can later take local forces: the player walking through grass, explosions, phase 15's flying hero.

**The data is already there.** Everything that moves is grown by the engine, and each mesh is built in its own frame with its foot at the origin and up along y. Height above the foot is what Crysis's main bending uses. The grass is built standing on y = 0, so a vertex's y is its height up the blade.

**Attached things must move together.** A tree's bark, leaves, climbers and epiphytes are separate meshes in one frame. If the sway is one function of the vertex's place in the tree's frame and the tree's own height, they all bend the same way and stay attached.

**No new vertex data at first.** The engine's vertex has a position, normal, uv and tangent, nothing spare. The sway comes from the height in the model's frame; the flutter's phase from the vertex's place in the world; the strength from the material. A per-vertex wind stream (Pivot Painter's data) is a later option if branches need to move on their own.

**Wind shelter:** the strength a material answers with stands in for the canopy's shelter: the crowns and the grass in the open move most, the understorey and the floor's cover least, as measured.

**Motion vectors every frame:** the motion pass gets last frame's wind (the same function at last frame's time), and every swaying object is drawn into it each frame. This is the cost to measure first.

**Shadows only near:** the nearest cascades redraw swaying casters each frame; the far ones keep still shadows, too coarse to show the sway.

## Plan

Following Horizon Zero Dawn.

1. **The wind force field** (`wind-field`): `WindSettings` (direction, speed, gusts' strength, size and speed, turbulence); a field in cascades round the camera (a near one about 128 m across, a far one about 1 km), each cell holding a spring per category (trees, plants, grass, hanging things: displacement and velocity), pushed each frame by the wind (travelling gusts and turbulence) and integrated in a compute pass; last frame's kept for motion vectors. The springs' frequencies from the measurements (trees about 0.3 Hz, plants about 1 Hz, grass about 2 Hz), each category's damping and response a setting. The maths mirrored in a shader library and tested; a debug view of the field.
2. **Swaying plants** (`wind-sway`): each material's wind category and response; every vertex shader samples its object's spring at the object's centre (trees and plants) or at its own place (grass, so gusts roll across a field). From each mesh's own frame, with no new vertex data: trees bend by height, branches by distance from the trunk, leaves flutter by their distance along their card from its stem; plants soft-clamped; grass with Horizon's ambient motion. Climbers and epiphytes use their tree's spring and frame, so they stay attached. Motion vectors from last frame's springs; the nearest shadow cascade animated, the rest still, as Horizon's.
3. **Hanging things** (`wind-hanging`): lianas, roots, vines and moss on the fourth category, swinging by their length below their anchor.

Each stage measured (the field's compute pass, the motion pass, the shadow cache) before the next.

**Stage 2 as built** (`wind-sway`): the sway's data is on each object (`ModelRenderer::sway`), not its material, since every tree is scaled differently and its climbers need the tree's height; the spring is read as a lean in radians, softly limited; the sway fades out between 80 and 400 m (the trees' impostors start at about 1 km); the floor's cover sways as grass does (a patch is many plants); leaves flutter more up the plant, not by their place along each card, which would need a coordinate the meshes do not keep. How it works: `docs/architecture/wind.md`.

**Stage 3 as built** (`wind-hanging`): each hanging vertex carries its two anchors, its share along and its arm (how far it hangs below the line between its anchors, or below its one anchor), read by the vertex shaders from a buffer by vertex number. Anchors in crowns move exactly as their tree there, so lianas stay on; the ground and the islands' rock are fixed anchors; the climbers' roots hang from their tree's own sway. The swing is the hanging spring's lean times the arm, a pendulum's arc. One spring frequency for every length at first; since `wind-chain-swing` (Nov 17 2026) the hanging spring comes in three bands (0.6, 0.2 and 0.08 Hz) each strand blends by its swing's length, so longer things swing slower, as chains do (5.2255 sqrt(L / g)). How it works: `docs/architecture/wind.md`.