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

## The mid layer's shape

- **Shade-tolerant trees** (the mid layer grows in the canopy's shade) have flatter crowns, fewer leaf layers and thinner stems than less tolerant ones (a Bornean study: [Plant Ecology 153](https://boris.unibe.ch/81050/1/PlantEcol_153_279.pdf)).
- Horn (1971)'s wide, flat "monolayer" crown for shade trees is a simplification: shade-tolerant species vary, some intermediate, some layered ([Smithsonian](https://repository.si.edu/bitstream/handle/10088/3723/Pearcy_Valladares_Wright_and_Paulis.pdf), [PubMed](https://pubmed.ncbi.nlm.nih.gov/27489160/)).

**What it means here:** the mid layer's trees are slender (trunks 20-40 cm), with shallow, flattish crowns a few metres deep and fewer clumps, standing under and between the canopy crowns. The canopy trees' crowns, from their 12 m fork to 38 m, leave no room under them for a layer 10-20 m tall; real canopy trees carry their crowns higher, from 50-60% of their height.

## The understorey

Gathered on Nov 14, 2026, after the user's look at the mid layer: "it doesnt really have that dense jungle look tho". What was missing, biggest first: nothing at eye level, temperate shapes (round puffs on poles; no palms, big leaves or vines), an open canopy, cool colours in dry light.

**How dense real understoreys are:**

- Small saplings (10-50 cm) in four Neotropical lowland forests: 4.8-7.5 a square metre at three sites, 0.7-1.6 at La Selva, where understorey palms and herbs cover much of the ground and crowd the saplings out ([Harms et al. 2004](https://repository.si.edu/bitstream/handle/10088/6754/Harms_Powers_and_Montgomery_2004.pdf)).
- Herb cover in Costa Rican forests: 2% (young) to 23% (intermediate age) of the ground in 25 m^2 plots, less again in old forest ([Diversity 2024](https://doaj.org/article/d11f4c42a5f74f0499a1a0301e311bed)).
- Some forests are fern-rich (Cocha Cashu, Peru), some palm-rich (La Selva), some sparse (Barro Colorado) (Harms et al.).

**How games build the plants:**

- **Palms:** a frond is a curved rachis (midrib) carrying leaflets in pairs; fronds placed round the crown by the golden angle, each varied ([PalmGenerator](https://github.com/edap/PalmGenerator), [SpeedTree](https://docs.unity3d.com/speedtree-modeler/manual/modeling-approach.html)). Crysis modelled each palm frond's leaves as geometry (GPU Gems 3, chapter 16).
- **Big leaves** (aroids, bananas): cut-out cards suit big leaves with wavy edges (Valve's wiki: [Vegetation](https://developer.valvesoftware.com/wiki/Vegetation)); small plants and herbs as cards, far plants as impostors.
- **Overdraw is the cost of cards,** more than triangles: a cut-out card's empty part still runs the pixel shader, and a pixel under many cards runs it many times. Cutting each card's mesh to its leaf's outline, a few more triangles for much less empty area, is the usual cure ([80.lv](https://80.lv/articles/tips-on-utilizing-vegetation-in-game-scenes), [Polycount](https://polycount.com/discussion/112248/alphas-vs-geo-for-foliage)); and a depth prepass with the cut-out test, so the main pass shades each pixel once (this engine has one). **(Forum and vendor sources, unverified numbers.)**

**What it means here:**

- **Plants built as geometry, not cards:** a palm frond's leaflets and a big leaf as meshes cut to their outline, so nothing is drawn where there is no leaf, and the depth prepass keeps the main pass's cost to the pixels seen. No CC0 library has palm or banana scans (`docs/TREES.md`), so their shapes are generated and their surface takes the scanned broad leaf's (`Leaf001`) colour and detail.
- **Three kinds** at mixed heights: understorey palms (2-6 m: a short or no stem, a fan of arching pinnate fronds), big-leaved herbs (0.5-2.5 m: a few huge leaves on long stalks, like elephant ears and young bananas) and ferns (0.3-1.5 m: a rosette of finely divided fronds).
- **Density:** thick along and round the path, where the view should close at eye level, thinning away from it; a few thousand plants in the patch, a few variants of each kind instanced.

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
