# Build log: Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 4 of 4)

## Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 4 of 4)

- `gpu-placement` d1 (`gpu-understorey`, Nov 20 2026): the understorey and palms drawn by the GPU's cells as the trees are, their entities retired; still the patch
  - **Variants by pick:** the understorey's 15 scans (4, 5 and 6 plants in its three files) are variants within 3 kinds, each plant choosing min(pick x n, n - 1). A look per (layer, kind) could not say that, so a look got a variant number and `GpuPlacedKind` its count and first slot (its spare words); `placedKindSlot` picks, mirrored in GLSL and checked on the GPU. Rejected: a kind a variant (the layer's shares change, so do the plants).
  - **Extra variants take the layer's top kind slots**, so a plant of any kind the looks name keeps its own slot; the table refuses variants reaching one.
  - **Entities saved:** about 28,000 (the understorey's 8,640 plants and the palms' 721 as seen mesh, stand-in and card each).
  - **Checked in a hidden copy:** `placement-compare` agrees in every view (camera 2,386 entries, a probe face, a shadow tile); the meadow looks as before; GPU frame 21.9 ms (laptop RTX 4070).
  - **Sway from the start:** the looks are given their sway when built (TestForest runs no frame), and anew with the plants' settings.
  - **Tested:** `placedKindSlot` against the entities' choice, a table's variants and refusals, the understorey's and palms' looks (models, levels, cards, sway), the field's looks after the trees' and swaying by the plants' settings.
- `card-fleck-glow` (Nov 20 2026): an understorey card ~18 m off glowed in a sun fleck while its mesh there was mostly shadowed
  - **Probed** (green/red for lit/shadowed in the main pass, one hidden copy): at each pixel's own place the card read an even ~half lit, its neighbours and the ground fully shadowed; the leaf points (the self-shadow's lookup) lay in the fleck and read lit. Same with dithered cascades, so not their blend. Ruled out before: slope bias, the stand-in's own shadow, contact shadows, the card's self-shadow, d1.
  - **Fix:** one sun-shadow value a card, averaged over 7 points spread over its plant (middle and +-0.5 radius along its axes, `impostorShadowSpread`), as engines shadow far cards coarsely; the far map once at the middle.
  - **Cost:** 7 one-tap lookups and one soft far lookup a card pixel, in place of a soft cascade lookup (32 taps) and a soft far one at each pixel's leaf.
  - **Left:** the mirror's cascades are not read for cards (they take the sun beyond).
- `gpu-placement` d2a (`near-tree-mirror`, Nov 20 2026): the big trees planted by the CPU a cell at a time, each crown's lianas its own
  - **Found:** the mesh pool only grows (`VulkanMeshPool`), so cells round the player could never drop their liana meshes. Weighed staging, mesh freeing, fixed slot meshes and GPU-drawn lianas; the user chose the GPU drawing them from records (scales best: about 100 bytes a liana against 20-40 KB). Research and plan in `docs/research/VINES.md`; rays for them with c3, as the trees are not in rays yet.
  - **Per-crown streams:** `chooseLianas` drew one stream across every crown, so one crown's lianas hung on every crown before it. Now each draws from the seed and its identity (layer and dither cell); different trees carry lianas than before, about as many (accepted).
  - **Neighbours across a cell's edge:** crowns stand above their feet, so planting the cell grown by `neighbourSpan` (30 m) finds every crown a loop can reach; `plantBigTrees`' order is the same in any box, so neighbours are drawn in the same order.
  - **Only the big trees' two layers planted:** they read nothing after them, so the same trees for a fraction of the work.
  - **Tested:** a crown's lianas the same among only its neighbours, a cell's trees the forest's, the patch's lianas the same from 64 m and 48 m cells. The last failed only at -O2: the test's `NearTreeSources` held a reference to a temporary settings struct (passed in debug by luck).
  - **Path-share test:** 0.95 against 0.8 over 60 crowns is within chance, and the new streams flipped it; now checked against a far share of 0.3.
