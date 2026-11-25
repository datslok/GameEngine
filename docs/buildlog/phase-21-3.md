# Build log: Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 3 of 3)

## Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 3 of 3)

- `vsm-breakdown` (Nov 25 2026): 4c-v, the pages' cost counted in F4 (`countCosts`, `virtual-shadows.md`), measured at the four spots with atomics.
  - **Draws kept whole are most of it:** 54-69% of the pages' triangles (rim 19.4 of 36.1 M, ring 37.3 of 64.7 M, wall 67.8 of 99.6 M, trees 45.6 of 66.0 M) from 18-29% of the casters' clusters. A swaying or reshaped draw lays its whole bounds into every level, so each cluster goes to every drawn page the tree touches: at the wall, fine levels 2-6 have 1-7 K clusters but 76-206 K pairs.
  - **Out of the camera's frustum:** 27-91% of the casters' clusters, but only 5-39% of the pages' triangles (38.7 M at the wall, which sees little); off-screen casters still shadow what is seen, so they stay.
  - **The rest, about a triangle a texel:** the other clusters take 1.0-1.7 pairs each a level; 16.7-31.8 M triangles in 1033-1272 pages of 16 K texels.
  - **Gotcha:** the page passes had no shader dependency files (lost in a branch switch), so a record change left them reading old offsets (13 pages marked); their `.spv` deleted and rebuilt. The 4c-iv timings came before any record change.
  - **8 planted bugs caught** (the level words swapped, not strided, the unseen pairs a word off, costs read uncounted, the switch not in the record or not visited, levels without pairs shown, millions rounded).
- `vsm-sway-bounds` (Nov 25 2026, in progress): 4c-vi, first the whole draws' costs split by why they are kept whole (`VSM SEEN`'s SWAYING, `VSM SHAPES`).
  - **Lianas are the most at the wall and among the trees:** 42.2 M of the pages' 101.6 M triangles at the wall, 27.4 of 66.0 M among the trees, from 4-6 K clusters (100-180 pairs each); 3.7-6.7 M at the rim and ring.
  - **Swaying without bones:** 10.2-22.6 M everywhere, from 233-535 K clusters at under one pair each; terrain patches 5.6-7.6 M; cards, ground-following and thinned draws none.
  - **3 planted bugs caught** (the swaying words read from the whole ones, the kinds not strided, their names swapped).
- `vsm-sway-bounds`, second step: clusters swaying without bones culled by their own spheres grown by the sway's reach (`windSwayReach`, `meshClusterSwayingPose`; `passes.meshClusterSwayReach`, on), in the camera's passes and the sun's casters.
  - **The worst lean, not this frame's:** the cluster passes read no springs and grass reads one a blade; the reach is exact maths, a few metres at most for a 2 m plant against the tens of metres a draw's bounds span.
  - **The bend's bound is loose:** keeping the length moves a vertex back by at most the sideways step, so 2x; a random bush never needed the scale, the flutter or the trunk distance to stay inside, so each got a case where it rules (a tall plant scaled, a short one fluttering hard, a low branch far out) and each cluster's reach is checked against its vertices' farthest across.
  - **9 planted bugs caught** (the bend without scale, no flutter, no bob, the reach 4x too loose, grass's or a plant's trunk distance short, the switch ignored, a grown cluster still seen whole); GPU tests written, not run.
