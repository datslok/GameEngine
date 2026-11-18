# Build log: Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 3 of 3)

## Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 3 of 3)

- `plant-fades` (Nov 18 2026): the scanned understorey plants' levels and cards cross-faded by a dither over a band of distance instead of swapped (research `docs/research/LOD.md`, Hiding the switches, item 2)
  - **Distance-based, not time-based (the user's choice):** a pure function of the view and the draw, so it slots into the GPU's candidates (both slots of a fading draw are already candidates) and the CPU's reference; Unreal's 0.5 s timed fade needs per-draw state kept between frames, reset whenever the still draws rebuild, which `cull-compare` could not follow. Its known cost: a plant standing in the band stays dithered, left to TAA and DLSS to blend.
  - **The band in the budget, not in metres:** a coarser level fits pixelError / (1 - band) a band's width before its switch, so one setting (0.2) scales with each level's error and the screen; the share is measured by the coarser's error, so a plant and its card fade by one number and split every pixel.
  - **Complementary pixels:** the coarser keeps noise below the field's step middle, the finer from it; 63 steps, never all or none, in 7 bits of the draw list entry, leaving 24 bits (16.7 M) of instance, checked.
  - **The prepass decides:** the main pass already shades only what the depth prepass kept (EQUAL), so only `cut_out_prepass.frag` (after its texture reads, keeping their derivatives) and the velocity pass discard. Hence cut-out materials only (the three scans and the cards are).
  - **Records read only for fading entries:** the flags pass marks a fading candidate with one more bit (`CAMERA_CULL_FLAG_BITS` 3 to 4) and the write pass works its field out again from the record, rather than spending 7 bits of the candidate's place.
  - **The camera only:** captures, the mirror and shadow tiles keep swapping (a turning dither in a shadow map would shimmer); the palms keep their card thinning.
  - **Tested:** the CPU's `levelFade` by hand-worked distances and a plant into its card; the packed choice equal to the CPU's over random scenes and views; the GPU's lists equal to `batchByMesh`'s entry for entry with fade fields; every pixel kept by exactly one level. GPU tests written (`level_fade_gpu_test`, `cull_records_gpu_test` cases), not run.
