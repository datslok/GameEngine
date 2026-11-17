# Trees that look good: research

Notes on how games and papers build good-looking trees, gathered for the jungle valley (ROADMAP phase 18, the forest item) after the user's look at the first grown trees on Nov 14, 2026: "i was thinking a lush jungle like crysis".

Each part records what the sources say, with their numbers, and then **what it means here**. The order to build things in is at the end.

Facts marked **(unverified)** are practitioner lore or from memory, not checked against a primary source. Check them before writing tests that depend on them. Several talks (GDC Vault) are paywalled; only their abstracts were read.

## Why the first trees looked wrong

The first trees (`tree-growth`, `games/forest/forest_trees`): space colonisation (Runions et al. 2007) in one oblate ellipsoid crown 28 m wide and 10 m deep, on a straight 32 m trunk; radii by Leonardo's rule with n = 2; one beech leaf on each 0.7-0.9 m cut-out card at every twig node; leaf normals bent towards the whole crown's outward direction.

They read as an umbrella on a pole (a stone pine), not a jungle tree. The user's references show:

- trunks forking low into a few thick, crooked limbs;
- gnarly, mossy bark;
- foliage in big rounded clumps, with depth and gaps between them;
- layers: trees of mixed heights over a dense understorey.

Each of these maps onto one of the causes below.

| What shows | Cause | Fix (part) |
|---|---|---|
| A flat, even lid | One ellipsoid, filled evenly | Clumps (1) |
| A pole under it | No apical control: nothing makes a few limbs win | Low fork, scaffold limbs (1) |
| Spindly limbs | Leonardo's n = 2 thins every fork | n of 2.5-3 (1) |
| Straight branches | No tropism | Gravity and light pulls (1) |
| A lattice of cards | One leaf a card at every node | Cluster cards on the clumps' shells (2) |
| One dome of light | One crown-wide normal | Clump normals (3) |

## 1. Structure: low forks and a few thick crooked limbs

**Apical control decides between a pole and a fork.** Palubicki et al. 2009, *Self-organizing tree models for image synthesis* ([paper](https://www.algorithmicbotany.org/papers/selforg.sig2009.small.pdf), read directly):

- Light flows down the tree: each bud's light Q is summed down the branches to the trunk.
- Resource flows back up: the trunk's base gets v = alpha Q_base (alpha = 2), split at each fork as v_main = v lambda Q_m / (lambda Q_m + (1 - lambda) Q_l).
- lambda sets the habit: **0.46-0.48 gives spreading, forked crowns; 0.52-0.54 a central leader** (their Fig. 7).
- A priority variant sorts branches by light with weights w_max = 1, w_min = 0.006, kappa = 0.35-0.5 (Fig. 9).
- **Fig. 10 is the references' look:** apical control removed early on the main stem but kept in the first-order limbs gives a short trunk and a few big limbs.

**Branch shedding gives clean trunks and few limbs.** A branch is dropped when its light divided by its size (in internodes) falls below a threshold (their section 4.4). This is what turns an even web into a few strong scaffolds.

**Tropism makes limbs crooked.** Each new segment's direction is a weighted sum of its default direction, the space colonisation direction (weight xi) and gravity (weight eta). A strong downward eta against a strong pull to light gives "highly gnarled branches" (Fig. 12c, d); varying eta over the tree's growth adds variety (Fig. 13).

**Space colonisation's numbers** in the same paper: perception angle about 90 degrees, occupancy radius 2 internodes, perception radius 4-6 internodes.

**Radii:** d^n = d1^n + d2^n with **n between 2 and 3**. n = 2 keeps area and leaves limbs thin at forks; 2.5-3 keeps the scaffolds fat against the twigs. Radii should remember shed branches (never shrink when one is dropped).

**Other models:**

- Weber and Penn 1995 ([SIGGRAPH](https://history.siggraph.org/?p=118727)): per-level branch counts, angles and curvature with variation. Its parameter names are **(unverified)**.
- Stava et al. 2014 ([paper](https://cs.purdue.edu/homes/bbenes/papers/Stava14CGF.pdf)): a developmental model with apical dominance and tropism fitted to example trees. Its parameter list was not read **(unverified)**.

**What it means here:** a hybrid of the paper's idea and our space colonisation, rather than simulating light flow (a synthesis, not a published method):

- grow the trunk to 25-40% of the tree's height, then 3-5 scaffold limbs, 30-60 degrees from vertical, spread round the trunk, kinked by noise;
- attractors in 5-12 separate clump volumes, one or two a limb, each limb colonising only its own; the space between them becomes the gaps;
- gravity and light terms in each growth step's direction;
- Leonardo's n at about 2.5.

## 2. Foliage: cluster cards, not single leaves

**SpeedTree's kinds of foliage** ([glossary](https://docs.unity3d.com/speedtree-modeler/manual/glossary.html)): leaf cards (facing the camera), leaf meshes (small pieces of geometry) and fronds (geometry along a branch's spine with a big-leaf texture). It prunes leaves that intersect each other ([leaf collision](https://docs.unity3d.com/speedtree-modeler/manual/leaf-collision.html)).

**Common game practice (unverified sizes):**

- an atlas of twigs with many leaves each (about 10-40 a card), cards about 0.5-2 m;
- cards only on the end twigs of each clump's outer shell, crossed or fanned at random turns, plus a few big cards inside so the crown cannot be seen through;
- tropical leaves are big (20-50 cm), so fronds or leaf meshes suit them: a stalk with a few big leaves.

**Overdraw is the cost limit** (pixels shaded several times over by overlapping cut-out cards):

- cut each card's outline close to its leaves' silhouette ([polycount](https://polycount.com/discussion/comment/2765685));
- one studio dropped full spherical normals on foliage because of the overlapping cards' cost ([Gaslamp](https://www.gaslampgames.com/?p=5817));
- Horizon Forbidden West shades foliage and cut-outs with a visibility buffer and deferred texturing in compute, for exactly this cost ([Guerrilla](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west); the talk itself is paywalled).

**Unreal Engine 5.7** (the Witcher 4 demo): whole 3D leaves through Nanite Foliage, with "assemblies" (branches instanced inside one asset) and voxels for the distance ([80.lv](https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7)); the Procedural Vegetation Editor builds trees from "Megaplant" presets ([Epic](https://dev.epicgames.com/documentation/unreal-engine/procedural-vegetation-editor-in-unreal-engine?lang=en-US)). Both experimental. The lesson: build a few branch-with-leaves units and instance them along the limbs.

**What it means here:** no CC0 leaf-cluster atlas was found (ambientCG had none). Draw our own at start-up, as the hanging moss's strands are drawn: twigs with 10-20 of `LeafSet024`'s leaves each, into an atlas of a few clusters. Cards only on each clump's outer twigs. Fewer cards with more leaf on each, so less overdraw than now.

## 3. Shading

**Normals at two levels.**

- The standard trick: take foliage normals from a smooth hull or sphere round it, so a cluster lights as one volume ([Unreal forum](https://forums.unrealengine.com/t/spherical-normals-for-trees-blender/98732)).
- Flatten the hull's bottom half into a cone, so undersides do not point straight down (same thread).
- SpeedTree sets leaf and frond normals relative to the whole tree or per generator ([SpeedTree](https://docs.unity3d.com/speedtree-modeler/manual/lighting-leaves-and-fronds.html)).

**What it means here:** blend each card's normal towards its clump's centre and then the crown's, so every clump has its own lit top and shaded underside; one crown-wide normal is what made ours one dome (our recommendation, not from a source).

**Translucency, as Crysis did it** ([GPU Gems 3, chapter 16](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)):

- back light = (E . L)^4 blended with wrapped N . L (x 0.6 + 0.4), times a painted thickness texture (128 x 128), times a back colour;
- leaves double-sided, the back face with the normal negated;
- Phong specular with a gloss map.

**Ambient occlusion and ambient light:**

- Crysis bakes occlusion into the vertex alpha and multiplies the colour by it; its hemisphere ambient blends between half and full by the normal's z.
- SpeedTree has per-vertex occlusion and an "AO dimming" that darkens leaves inside the crown ([leaf generator](https://docs.unity3d.com/speedtree-modeler/manual/leaf-generator-properties.html)).
- A cheap version: occlusion from how deep a leaf sits in its clump times how deep the clump sits in the crown.

**Colour and gloss (unverified numbers):** shift each tree's and each clump's hue, saturation and brightness by about 5-10% against repeats; glossy tropical leaves want a real specular lobe, roughness about 0.3-0.4.

**What it means here:**

- translucency is the "light through leaves" stage already planned;
- per-vertex occlusion needs a new vertex attribute (`MeshVertex` has none for it), a renderer change; the screen-space ambient occlusion covers part of it meanwhile;
- colour per tree can be a material tint per instance.

## 4. Bark

- **Moss** by facing up (normal . up), stronger low on the trunk and on limbs' upper sides: standard world-aligned blending **(unverified, no primary source)**. Here: the rocks' projected material already lays moss on what faces up (`groundTopLayer`), so bark can reuse it.
- **Gnarl:** low-frequency noise displacement and twist along the tube; buttress fins at the base.
- **Buttresses** grow with crown exposure and tree size, measured by the height where the fin meets the trunk ([Frontiers 2019](https://www.frontiersin.org/journals/forests-and-global-change/articles/10.3389/ffgc.2019.00032/full)).
- **Texture scale** fixed in the world: v along the length, u by the real circumference, so thick limbs are not stretched **(unverified, standard practice)**. Here: done (`TubeOptions::wholeTilesRound`).

## 5. Jungles, density and cost

**The references are not typical emergents.** Real emergents are slender with tall clear trunks: Menara, 100.8 m tall, is 2.12 m across above its buttresses, and many emergent crowns are hemispheres, their branches radiating from the top of the trunk ([Frontiers 2019](https://www.frontiersin.org/journals/forests-and-global-change/articles/10.3389/ffgc.2019.00032/full)). Low forks and crooked limbs are open-grown or edge trees (figs, rain trees). A jungle has both.

**Proportions:**

- In Brunei, a crown's depth relative to its tree falls as the tree's height rises ([Cambridge](https://www.cambridge.org/core/product/E9CE145E7291C2328C2E5B7AD085ED21)).
- At La Selva, crowns widen sharply once a tree reaches the canopy (same source).

**Layers** (from educational sources; Richards 1996 not read):

| Layer | Height |
|---|---|
| Emergents | 45-50 m and up, over 60 m in Asia ([Banin 2012](https://www.geog.leeds.ac.uk/projects/amazonica/papers/Banin_2012_What%20controls%20tropical%20forest%20architecture.pdf), [NSW](https://www.nationalparks.nsw.gov.au/-/media/visitor/files/pdf/education/rainforests-of-nsw/rainforest-layers.pdf)) |
| Canopy | 20-40 m |
| Understorey | 3-10 m |

**Density without the cost exploding:**

- Horizon Zero Dawn places its vegetation on the GPU, layer by layer, from density maps ("ecotopes"): 64 pipelines, one a layer, about 32 bits of world data a square metre, over 500 kinds of asset ([GDC 2017 slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)).
- A few variants instanced, cards at lower detail further off, then octahedral or hemi-octahedral impostors; one implementation uses 24 directions 15 degrees apart plus one from above ([blog](https://app.cinevva.com/blog/2026-05-04-open-world-browser-part-19-imposters)).

**Vines and epiphytes:** no primary game source found. Here: the islands' hanging strands and moss cards can be reused on trunks and limbs.

## The order to build in

Most effect for the effort first; each step looked at before the next.

1. **Shape:** clumps (5-12 separate attractor volumes), a low fork into 3-5 kinked scaffold limbs, Leonardo's n about 2.5, gravity and light in the growth.
2. **Foliage:** leaf-cluster cards drawn at start-up, only on the clumps' outer shells, with clump normals (cone-flattened underneath).
3. **Bark and colour:** moss on the bark from the rocks' material, a colour shift per tree; later noise and twist in the bark.
4. **Light through leaves:** Crysis's back light, its own branch (already planned).
5. **The jungle patch:** two or three kinds (a tall clear-trunked emergent, a crooked low-forked canopy tree, an understorey palm or shrub) at mixed heights, vines and epiphytes from the islands' code.
6. **Later, when heavy:** per-vertex occlusion (a renderer change), impostors, placement on the GPU.

## Not verified

- Typical card sizes and leaves a card, colour shift sizes, leaf roughness and the moss blend's details: practitioner lore.
- Weber and Penn's and Stava et al.'s parameter lists: not read.
- Ghost of Tsushima's talk covers only grass and wind ([PlayStation blog](https://blog.playstation.com/?p=345372)).
- Horizon Zero Dawn's vegetation art talk (Sanders, GDC 2018) and Horizon Forbidden West's (McLaren): paywalled.
- The rainforest layers' heights: educational sources, not Richards.
- No CC0 leaf-cluster atlas found (ShareTextures claims CC0, licence per item).
