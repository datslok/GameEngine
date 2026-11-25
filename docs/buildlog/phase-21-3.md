# Build log: Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 3 of 3)

## Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 3 of 3)

- `vsm-breakdown` (Nov 25 2026): 4c-v, the pages' cost counted in F4 (`countCosts`, `virtual-shadows.md`), measured at the four spots with atomics.
  - **Draws kept whole are most of it:** 54-69% of the pages' triangles (rim 19.4 of 36.1 M, ring 37.3 of 64.7 M, wall 67.8 of 99.6 M, trees 45.6 of 66.0 M) from 18-29% of the casters' clusters. A swaying or reshaped draw lays its whole bounds into every level, so each cluster goes to every drawn page the tree touches: at the wall, fine levels 2-6 have 1-7 K clusters but 76-206 K pairs.
  - **Out of the camera's frustum:** 27-91% of the casters' clusters, but only 5-39% of the pages' triangles (38.7 M at the wall, which sees little); off-screen casters still shadow what is seen, so they stay.
  - **The rest, about a triangle a texel:** the other clusters take 1.0-1.7 pairs each a level; 16.7-31.8 M triangles in 1033-1272 pages of 16 K texels.
  - **Gotcha:** the page passes had no shader dependency files (lost in a branch switch), so a record change left them reading old offsets (13 pages marked); their `.spv` deleted and rebuilt. The 4c-iv timings came before any record change.
  - **8 planted bugs caught** (the level words swapped, not strided, the unseen pairs a word off, costs read uncounted, the switch not in the record or not visited, levels without pairs shown, millions rounded).
