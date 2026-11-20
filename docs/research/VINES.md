# Vines and epiphytes

Research for the jungle patch's next item (ROADMAP phase 18): lianas, climbers and the plants growing on trees, how real forests hold them, how others build them, and a plan for this engine. Written Nov 15 2026. Facts not checked against a source are marked **[unverified]**, conclusions of our own **[derived]**.

## How many, and where

**Lianas** (woody vines), from the best census, 50 ha on Barro Colorado Island, Panama ([Schnitzer et al. 2012](https://pmc.ncbi.nlm.nih.gov/articles/PMC3528767/)):

| Stem across | Lianas a hectare | Share |
|---|---|---|
| 1-2 cm | 475 | 50% |
| 2-5 cm | 395 | 42% |
| 5-10 cm | 65 | 7% |
| 10-20 cm | 8 | 1% |
| 20 cm and over | 0.5 | under 0.1% |

- **944 lianas a hectare** (1,350 rooted stems with clones). The thickest reach 37-55 cm.
- **Clumped:** they gather round old treefall gaps more than trees do, and forest edges carry more than the interior ([Laurance](https://repository.si.edu/bitstream/handle/10088/1469/Laurance_Ecology.pdf)). So the path and the clearing should be the most vine-laden.
- **Trees carrying lianas:** 48-75% on Barro Colorado, 86% in a Bolivian liana forest ([Pérez-Salicrup 2001](https://complete.bioone.org/journals/biotropica/volume-33/issue-1/)), 42% in central Africa.
- **For here [derived]:** lianas on 50-85% of the canopy trees, 1-5 each; about half thin (1-2 cm), one in a hundred a thick rope of 10 cm or more.

**How they climb** (Putz 1984): stem twiners, branch twiners, root climbers, tendril climbers and scramblers. Twining is the commonest (58% of species in one Chinese forest, [PMC2685309](https://pmc.ncbi.nlm.nih.gov/articles/PMC2685309)). The kind decides where it grows:

- **Tendril climbers** need thin supports: saplings, the understorey, a crown's twigs.
- **Twiners** fail on thick ones: Darwin's wisteria could not climb a 15 cm post ([PMC4363473](https://pmc.ncbi.nlm.nih.gov/articles/PMC4363473)).
- **Root climbers** (philodendrons, monsteras, figs) take any trunk. They are the ones hugging the big trees.

**What they look like:**

- **Loops between crowns:** a liana climbs young, then as trees grow and fall it is left hanging in long arcs between crowns. Few die when their host falls; they climb again at the gap's edge.
- **Rope lianas:** old stems hanging nearly straight from a crown to the ground, often slanting, bare below the canopy with all their leaves up in the crowns.
- **Root climbers** (hemiepiphytes, [Zotz 2013](https://repository.si.edu/bitstream/handle/10088/21559/stri_Zotz_2013_Annals_of_Botany.pdf)): short anchor roots gripping the trunk, and long unbranched feeder roots hanging straight down to the soil, from 1.6 m to 22 m up; slow ones end in mid air. Young monsteras and philodendrons press small flat leaves against the bark, overlapping like tiles ("shingling"), then switch fairly abruptly to big leaves on stalks standing off the trunk ([aroidpedia](https://www.aroidpedia.com/monstera-morphology)).
- **Strangler figs:** roots fused into a net round the host's trunk, more roots hanging free from the crown.

**Epiphytes** (plants growing on trees) by Johansson's zones: I the trunk's foot, II the trunk to the first branch, III the crown's base (thick inner limbs and crotches), IV the middle crown, V the outer twigs.

- They are fewest at the trunk's foot and most in the middle and upper crown ([Frontiers 2022](https://www.frontiersin.org/articles/10.3389/ffgc.2022.1007473/full)).
- **For here [derived]:** big rosettes (bird's-nest ferns, staghorns, tank bromeliads) in crotches and on the upper sides of thick, nearly level limbs in zone III, where humus gathers; moss and small ferns on limbs' upper sides and the damp lower trunk; small ones on the outer twigs.
- **How many:** a Panama plot had 13,099 epiphytes on 389 of 1,358 trees, about 34 on each tree carrying any ([PMC2803392](https://pmc.ncbi.nlm.nih.gov/articles/PMC2803392)); trunk thickness predicts them best. For "lush": 20-60 on a big tree, 0-5 on a small one.
- **Sizes:** a bird's-nest fern's fronds are 0.5-1.5 m long and 10-20 cm wide, a funnel 0.6-1.5 m across of strap-shaped, glossy, undivided fronds with dark midribs ([San Diego Zoo](https://animals.sandiegozoo.org/node/389)). Tank bromeliads 20-60 cm **[unverified]**.

## Shapes

- **A liana between two holds** hangs as a catenary, y = a cosh(x / a). No source gives lianas' sag; by eye, loops between crowns sag 10-40% of their span and dropped ones more **[unverified]**. A woody stem resists bending, so a thick one curves more stiffly near its holds than a chain (an elastica) **[derived]**.
- **Twiners' helices:** on a thicker support the climbing angle falls while the coil's curvature stays the same; past about 3.3 times the plant's own coil radius it loses its grip ([plus.maths.org](https://plus.maths.org/content/going-world)).
- **Swing (for wind later):** a chain hung from one end swings with period 5.22 sqrt(L / g): 7.5 s for a 20 m rope liana, 3.7 s for 5 m. Slow, wide, small sway reads as heavy.

## How others build them

**Papers:** every method is the same idea, growth by particles or space colonisation with a pull towards the host's surface, a pull towards light and gravity:

- Benes & Millán 2002, "Virtual climbing plants competing for space": particles sensing a voxel grid of space and light, several climbers avoiding each other ([PDF](https://www.cs.purdue.edu/homes/bbenes/papers/Benes02CA.pdf)).
- Luft's Ivy Generator 2007, the one most used (Blender's IvyGen): each step weighted by a direction, randomness, adhesion to nearby surfaces, an up vector and gravity; leaves turned by the smoothed adhesion ([site](https://graphics.uni-konstanz.de/~luft/ivy_generator/)).
- Wong & Chen 2016: tendrils searching for a host and coiling round it ([CGF](https://diglib.eg.org/handle/10.1111/cgf12736)).
- Hädrich et al. 2017, "Interactive modeling and authoring of climbing plants": connected particles reacting to light, obstacles and wind, in real time, up to 20 plants of 25,000 branches ([PDF](https://www.cs.purdue.edu/homes/bbenes/papers/Haedrich2017EG.pdf)).

**Games and tools:** no shipped game has published how its vines were built.

- **Crysis:** the jungle's layers were grass, plants, bushes, trees and vines; its vegetation bends in two layers in the vertex shader ([GPU Gems 3, ch. 16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)) and CryEngine had rope physics for "bendable vegetation". How its vines were made is not documented.
- **Assassin's Creed Shadows:** an ivy tool grown into a 3D vine growth system run inside the editor through Houdini ([80.lv](https://80.lv/articles/check-out-this-ivy-scattering-tool-created-with-houdini)): studios grow vines procedurally rather than placing them by hand.
- **SpeedTree's vine generator:** a branch with a droop simulation (gravity, stiffness, friction, pins at the start, end or random points), colliding with the tree; for games, vines that follow their parent branch then swoop down ([docs](https://docs.unity3d.com/speedtree-modeler/manual/create-low-poly-vines-for-games.html)). A jungle artist's recipe: vine trunks scattered round the trunk with an upward force, leaves along them, vine leaves given less wind than the crown's ([80.lv](https://80.lv/articles/creating-a-jungle-environment-with-speedtree-megascans-ue4/)).
- **Unreal and Houdini tools:** splines swept into tubes along catenaries or Verlet ropes, leaves scattered along them.

**Rendering:**

- **Tubes:** 3-4 sides for thin hanging vines, 6-8 for thick ones seen up close, segments 0.25-0.5 m on droops, finer on coils **[unverified, practice]**.
- **Leaves:** turned to the surface they cling to, a few millimetres off it; hanging lianas leafy only where they enter a crown.
- **Thin and long:** wires and hair alias. Persson's "Wire Antialiasing" (GPU Pro 5, 2014) widens a wire to at least a pixel and fades it by its true width over the widened one ([gems db](https://www.gameenginegems.com/gemsdb/article.php?id=1223)). The same thinness makes vines flicker in coarse shadow cascades.

## What the engine has

- **Hanging strands** (`island_hangings`): a strand from an anchor drooping as a heavy rope, swept into a bark tube (`appendTube`), ivy leaves (`LeafSet017`) on cut-out cards along it (`appendLeafCards`). Exactly a rope liana's or a feeder root's pieces.
- **Moss cards** (`hanging_moss`) for moss on limbs.
- **Palms' fronds** (`palms`): strap leaves bent as cantilevers round a crown, their picture drawn at start-up: most of a bird's-nest fern.
- **Trees:** grown by space colonisation, a few variants placed many times; the branch skeleton is used while growing and then dropped, so it would be kept for anything to climb or sit on.

## The plan for this engine

**Where they grow:** climbers and epiphytes belong to a tree, so they are grown with each tree variant into its meshes. They repeat with the variant, as the tree already does, at no extra draw cost. Lianas between two trees belong to the pair, so they are grown per placement into drawn-only meshes of their own, a chunk at a time, as the islands' hangings are.

In stages, each its own branch, judged by eye:

1. **Lianas** (`jungle-lianas`): loops between neighbouring crowns (a catenary stiffened near the holds, sagging 10-40%) and ropes from a crown to the ground. On about two thirds of the canopy trees, 1-5 each, more along the path and the clearing; thickness drawn from the census (mostly 1-5 cm, a rare 10-20 cm cable). Bark tubes of 4-6 sides; ivy leaves only where they enter a crown. Shadows in the nearer cascades only.
2. **Climbers** (`jungle-climbers`): root climbers wound up the big trunks with shingle leaves pressed to the bark low down and big stalked leaves higher (from `Leaf001`), their feeder roots hanging straight down, some ending in mid air. Grown on each variant's trunk from its kept skeleton.
3. **Epiphytes** (`jungle-epiphytes`): bird's-nest ferns and bromeliads in crotches and on the tops of thick limbs (rosettes from the palms' strap leaves), moss along the limbs' upper sides; 20-60 on a big tree, few on small ones.

Each stage measured (triangles, shadow cost, the frame) before the next. Wind comes after, with the hanging ones swinging slowly by their length.

**Later, if they flicker:** Persson's wire anti-aliasing for thin vines, in the main and shadow passes.

## At valley scale: lianas drawn by the GPU

Built once at start-up, the patch's lianas are meshes: about 20-40 KB each (a tube of 4-6 sides every 0.5 m plus its ivy). Across e1's 1 km of trees that is tens of thousands of lianas, gigabytes, and the mesh pool only grows (`VulkanMeshPool`: appended, never freed), so cells could not drop theirs. The user's choice (Nov 20 2026): the GPU draws them from small records, which scales best.

**How others draw thin procedural strands:**

- **A template mesh bent by a record a strand** (Ghost of Tsushima's grass, GDC 2021, [session](https://gdcvault.com/play/1027214/contactUs); [a reimplementation](https://cainrademan.github.io/Unity-Grass/)): one shared blade mesh whose vertices carry only their place along the blade; the vertex shader reads the blade's record from a buffer by its instance and puts each vertex on a Bezier curve, wind moving its control points. 15 vertices near, 7 far, crossed between.
- **Strands tessellated from points** (Frostbite's hair, [SIGGRAPH 2019](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf), [blog](https://www.ea.com/frostbite/news/frostbite-hair-rendering-and-simulation-2)): about 25 points a strand, drawn as a strip; far away fewer, thicker strands (AMD's TressFX LOD, [GDC 2014](https://fr.slideshare.net/DevCentralAMD/grass-fur-and-all-things-hairy-amd-at-gdc14)).
- **Hanging cables as catenaries in the shader** ([Godot catenary](https://github.com/Donitzo/godot-catenary); DreamWorks' cobwebs used a parabola as cheaper, [talk](https://research.dreamworks.com/wp-content/uploads/2018/07/63-lee-Edited.pdf)): the curve's constants found once off the GPU, each vertex placed in closed form.
- **Thin wires stay a pixel wide** (Persson's wire AA, above; [Unigine's vertex-shader form](https://developer.unigine.com/forum/topic/1524-solved-mesh_wire_base-material/)): widened to a pixel in the vertex shader, faded by a dither (not blending, which depth effects would break) by true width over drawn width.

**The plan:**

| Part | How | Cost grows with |
|---|---|---|
| Record a liana | Holds, length, radius, the catenary's `a` and lowest point solved on the CPU (`catenaryStrand`'s halving), its two trees' sway (foot, height, scale): about 100 bytes, against 20-40 KB of mesh | lianas held |
| Records a cell | A fixed range a CPU cell slot (as the GPU's cells), written when the cell is made; a dropped cell's range is simply reused | cells made |
| Tube | One shared template tube (rings × sides, each vertex its ring's share along and its angle round), a level a ring count (about 48 / 16 / 6 rings, 6 / 4 / 3 sides) chosen by the existing levels on the record's box; the vertex shader puts each ring on the catenary in closed form (`cosh`, `sinh`), mirrored in a `.glsl` library | lianas drawn × their level's rings |
| Wind | The hanging data (`HangingVertex`: along, arm, swing length) worked out in the shader from the ring's share and the curve; the anchors' trees carried in the record, so the 65,534-anchor table goes | as the tube |
| Ivy | A template of leaf cards, each corner's card and place in its uv; the runs through either crown measured on the CPU into the record, each card placed by the inverse of the leaves' count along it (dense in the crowns, sparse between, every card used), the rest collapsed to nothing; templates of 16-256 cards, the smallest holding a liana's; every other card dropped (the rest grown) far off | lianas drawn × their template's cards |
| Passes | The same `triangle_vertex` path every pass uses (prepass, main, shadows in the nearest 2 cascades, velocity from last frame's wind): one shaping function as `thinnedVertex` is | none extra |
| Thin | Persson's widening to a pixel and dither fade, if they flicker | none extra |

- Ray tracing (the user's choice, Nov 20 2026: with the placed trees in c3, not before; the trees are not in rays yet either): a compute pass writes each near liana's coarsest tube (6 rings x 3 sides, about 36 triangles, about 1.5 KB) into its cell slot's buffer when the cell is made, a fast-built structure a slot, ivy cards not opaque; rest pose, as the trees. So the catenary stays in a `.glsl` library a compute pass can call. Weighed: boxes with the ray queries' own hit test (tiny, but slow and in every tracing shader); NVIDIA's linear swept spheres (exact and fast, Blackwell only: at most an extra on top).
- Agreement: the C++ mirror places the same points, so the GPU test compares positions and a CPU test the records against today's `catenaryStrand`.
