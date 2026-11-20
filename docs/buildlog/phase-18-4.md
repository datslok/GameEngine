# Build log: Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 4 of 4)

## Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 4 of 4)

- `gpu-placement` d1 (`gpu-understorey`, Nov 20 2026): the understorey and palms drawn by the GPU's cells as the trees are, their entities retired; still the patch
  - **Variants by pick:** the understorey's 15 scans (4, 5 and 6 plants in its three files) are variants within 3 kinds, each plant choosing min(pick x n, n - 1). A look per (layer, kind) could not say that, so a look got a variant number and `GpuPlacedKind` its count and first slot (its spare words); `placedKindSlot` picks, mirrored in GLSL and checked on the GPU. Rejected: a kind a variant (the layer's shares change, so do the plants).
  - **Extra variants take the layer's top kind slots**, so a plant of any kind the looks name keeps its own slot; the table refuses variants reaching one.
  - **Entities saved:** about 28,000 (the understorey's 8,640 plants and the palms' 721 as seen mesh, stand-in and card each).
  - **Checked in a hidden copy:** `placement-compare` agrees in every view (camera 2,386 entries, a probe face, a shadow tile); the meadow looks as before; GPU frame 21.9 ms (laptop RTX 4070).
  - **Sway from the start:** the looks are given their sway when built (TestForest runs no frame), and anew with the plants' settings.
  - **Tested:** `placedKindSlot` against the entities' choice, a table's variants and refusals, the understorey's and palms' looks (models, levels, cards, sway), the field's looks after the trees' and swaying by the plants' settings.
