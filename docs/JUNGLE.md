# A jungle patch: research

Notes on how real rainforest is built and how shipped games draw dense jungle, gathered on Nov 14, 2026, for phase 18's jungle patch along the path past the Ring. The trees' own growth is in `docs/TREES.md`; this doc is about layers, sizes, density and the look.

Each part records what the sources say and then **what it means here**. Facts marked **(unverified)** are from memory or secondary sources; check them before relying on them.

## Why the first trees read small

The user doubted the trees were 30 m tall. Measured on the grown meshes (Nov 14 2026): bark tops 27.6-29.0 m, leaves to 29.5-30.8 m, crowns 13-14 m out from the trunk, each tree then scaled 0.85-1.15. So they are 25-35 m tall, and their trunks (0.75 m across at the fork, about 1.2 m at the foot) are normal for that height (see Sizes).

What makes them read small:

- **Leaf size.** The cards carry 30-40 cm leaves (the user's choice over real 15-25 cm). Leaves are the one part whose size everyone knows, so oversized leaves shrink the whole tree in the eye.
- **Nothing in between.** A lone 30 m tree beside 600 m pillars, with no understorey or mid-sized trees to step the eye up, reads as a bush.

## Sizes

| Layer | Height | Source |
|---|---|---|
| Emergents | 45-60 m and more | `docs/TREES.md` (Banin 2012, NSW) |
| Canopy | 20-40 m | same |
| Understorey | 3-10 m | same |

- **Height against trunk:** 845 tropical trees of 52 species spanned 10-208 cm across at breast height and 8-67 m tall, crown areas 1.4-1,495 m^2 ([Research Square](https://www.researchsquare.com/article/rs-2209593/v1)). Menara, 100.8 m, is 2.12 m across above its buttresses (`docs/TREES.md`).
- **Emergents' crowns:** above about 25 m, six Costa Rican emergent species' crowns were about 0.49 of the tree's height across **(unverified: from a truncated abstract)** ([ResearchGate](https://www.researchgate.net/publication/231749507_Allometry_of_emergent_tree_species_from_saplings_to_above-canopy_adults_in_a_Costa_Rican_rain_forest)). A 55 m emergent: a crown about 27 m across.
- **Region matters:** at the same trunk, Asian, African and Guyana Shield trees are taller than most Amazonian ones ([Feldpausch 2011](https://bg.copernicus.org/articles/8/1081/2011/)). Lianas make trees about 1.8 m shorter at the same trunk ([PLOS ONE](https://journals.plos.org/plosone/article/file?id=10.1371%2Fjournal.pone.0255197&type=printable)).

## Density

Stems per hectare in a Nigerian rainforest reserve ([Omo Forest Reserve](https://next.sciencegate.app/document/10.30955/gnj.000247)):

| Trunk across | Stems a hectare | One every |
|---|---|---|
| 5-20 cm (understorey, saplings) | 400-1,134 | 3-5 m |
| 20-40 cm (mid layer) | 90-174 | 8-10 m |
| over 40 cm (canopy, emergents) | 8-17 | 25-35 m |

An Ecuadorian hectare held about 600 trees over 10 cm **(unverified: a fragment)**.

**What it means here:** a jungle's density is mostly small and middle stems. The big trees are 25-35 m apart, their crowns touching; what closes the view is a mid layer every 8-10 m and an understorey every few metres, then palms, ferns and vines.

## How games draw it

**Crysis** (Sousa, GPU Gems 3, chapter 16: [NVIDIA](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)):

- "Thousands of different vegetation objects, often covering the entire screen"; no counts given.
- **Leaves:** palms get a modelled frond per leaf; bigger trees several leaves on one low-poly card; leaves alpha-tested, grass blended.
- **Light through leaves:** a painted thickness map times a back term: the view looking into the light, `saturate(dot(E, -L))^4`, blended by an artist's dial with wrapped back lighting `saturate(dot(-N, L) * 0.6 + 0.4)`, times the shadow averaged over lights.
- **Ambient:** a hemisphere, `lerp(0.5 ambient, ambient, N.z * 0.5 + 0.5)`; flat ambient "looks very boring".
- **Wind:** main bending (the whole plant, bend `height * scale`, kept on a sphere so it never stretches) and detail bending (leaf edges and leaves, four smoothed triangle waves at 1.975, 0.793, 0.375, 0.193), with vertex colours holding edge stiffness, a phase per leaf and leaf stiffness. Too much bending "will ruin the illusion".

**Horizon Zero Dawn** (`docs/TREES.md`): vegetation placed on the GPU layer by layer from density maps, over 500 kinds of asset. **Horizon Forbidden West** sped up foliage and cut-outs with deferred texturing ([GDC](https://gdcvault.com/play/1027553/Adventures-with-Deferred-Texturing-in)).

**Ghost of Tsushima:** procedural grass generated near the camera, distant foliage swapped for textures **(unverified, secondary)** ([GDC](https://gdcvault.com/play/1027214/Advanced-Graphics-Summit-Procedural-Grass-in)); few kinds of plant per biome, so each area has its own look ([PlayStation Blog](https://blog.playstation.com/?p=333111)).

**Uncharted 4:** jungles are hard to read because of their clutter of branches, bushes, twigs, vines and rocks; light and shadow isolate the focal points ([Game Informer](https://gameinformer.com/b/features/archive/2015/01/21/the-secret-depth-of-uncharted-4-s-art-design.aspx)).

**Avatar: Frontiers of Pandora** (Snowdrop): "thousands of assets in one frame" through its scattering system; no technical talk on vegetation found ([GameSpot](https://Gamespot.com/articles/avatar-frontiers-of-pandora-looks-beautifully-alien-in-new-snowdrop-engine-tech-showcase/1100-6493242/)).

## The plan for this engine

In stages, each its own branch, the user judging each by eye:

1. **The tall layers** (`jungle-canopy`): the canopy trees to 35-40 m; a second species, emergents 50-60 m with tall clear trunks and a wide crown spread from the top (about half their height across), a few per patch 25-35 m apart; leaves back towards real size (15-25 cm), the user to judge.
2. **The mid layer** (`jungle-midstorey`): a slender species 10-20 m, trunks 20-40 cm, one every 8-10 m between the giants, so the canopy closes and the eye has steps.
3. **The understorey** (`jungle-understorey`): palms with modelled fronds (Crysis's choice for palms), ferns and broad-leaved plants grown from `Leaf001`, one every few metres in the patch.
4. **Vines and epiphytes:** the islands' hanging strands and moss reused on trunks and limbs.
5. **Light through leaves:** Crysis's back term (or a physically based thin translucency), its own branch.
6. **Wind:** main and detail bending, the same in every pass.

**Cost:** a patch of 100 x 300 m (3 ha) at these densities holds about 50 big trees, 400 mid trees and thousands of understorey plants. Instancing a few variants keeps memory small; the frame cost (cut-out overdraw, shadows) is to be measured at each stage, with level of detail and impostors (already on the roadmap) when it is needed.
