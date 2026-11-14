# Still and moving shadows: research

Notes on how shipped engines keep shadow maps cached for what stands still while drawing what moves every frame, gathered on Nov 14, 2026, after the user saw the forest's drifting rocks' far shadows jump twice a second.

Each part records what the sources say and then **what it means here**. Facts marked **(unverified)** are from memory or informal sources; check them before relying on them.

## The problem

The sun's far shadow map (`ShadowKind::Far`, docs/ARCHITECTURE.md, The far shadow map) covers 5 km round the camera at 2.7 m texels on Ultra and holds every big caster: the valley's pillars, islands, the Monolith, the trees. A few hundred drifting rocks (2-20 m) move round the islands at up to about 6 m/s.

- Redrawing the whole map every frame cost 2.3 ms of 18.5 (the RTX 4070 Laptop).
- So it was kept up to half a second while casters only moved (`farShadowRefreshSeconds`), and the rocks' shadows jumped about 3 m twice a second.

## What engines do

| Engine | Design | Source |
|---|---|---|
| Insomniac (SIGGRAPH 2012, Acton, Day, Hastings) | a cached map of "static" geometry (anything that has not moved for a while, say 5 s), **copied** each frame into a temporary map, the non-static geometry drawn over the copy; the cache **scrolled** as the camera moves, only uncovered edges redrawn | [slides](https://advances.realtimerendering.com/s2012/insomniac/Acton-CSM_Scrolling(Siggraph2012).pdf) |
| Unity HDRP, mixed cached shadow maps | static casters into a cached atlas when asked; every frame the cached map **blitted** into the dynamic atlas and the dynamic casters drawn on top; costs both atlases' memory and the blit | [docs](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.1/manual/Shadows-in-HDRP.html), [PR](https://github.com/Unity-Technologies/Graphics/pull/1559) |
| Unreal Engine 5, virtual shadow maps | 128 x 128 pages cached until invalidated; "two depth copies: one static, one dynamic" per page, merged where dynamic geometry moves; movable meshes that stay still join the static cache | [Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine) |
| Unreal 4/5, movable lights | caching static primitives' shadows cut "upwards of 14 ms" to "around 1 ms" in a moderate scene | [Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/movable-light-mobility-in-unreal-engine) |
| CryEngine 3, cached shadows | far cascades drawn once and kept (past 16 km "at almost no cost", about 130 MB); movers **not** updated in them, a per-entity flag sends a mover to the regular cascades instead | [docs](https://cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21267738) |
| Cyberpunk 2077 (SIGGRAPH 2021) | up to 4 cascades for near and dynamic things; 4 distant levels, each 4 times the last, the largest over the whole 256 km^2 map, drawn over several frames every few seconds, movers not redrawn into them; local lights split into static and dynamic slices | [paper](https://history.siggraph.org/wp-content/uploads/2022/06/2021-Talks-Dybisz_Shadows-Optimizations-in-Cyberpunk-2077.pdf) |

Informal numbers:

- In Unity's pull request, a reviewer's 11-spotlight test went from about 60 to 40-50 fps, put down mostly to copying an 8k atlas every frame **(unverified, a reviewer's number)**.
- Separating static and dynamic pages doubles virtual shadow maps' physical page pool, according to an unofficial wiki **(unverified)**.
- Whether Unreal merges the two copies before or during sampling **(unverified)**.

## Two ways to combine still and moving

**(a) Copy the still depth, then draw the movers on top** (Insomniac, Unity, probably Unreal's pages):

- One map, so filtering, the soft shadows' blocker search, the bias and the fog's lookup stay exactly as they are; a mover's shadow over a still one is right, since the depth test keeps the nearest surface.
- A copy every frame and a second image's memory; movers drawn at the still map's resolution.
- Copying only the patches the movers touched is far cheaper than the whole map: depth copies between images of one format with `vkCmdCopyImage`, many rectangles in one call.

**(b) Two maps, combined when sampled:**

- No copy, and the movers' map can be small, fitted tightly round them, with finer texels.
- Two lookups and two filters in every reader (main pass, fog, probe capture); the two maps' visibilities combined (a minimum), since their depths are in different projections; a blocker search in both; seams where the movers' map ends; a bias each.

No source compares the two head to head.

## Following the camera

- Snap the still map's centre to whole texels (ours snaps to an eighth of its distance, `farShadowSnapShare`).
- Insomniac scrolls the cache, moving its contents by the camera's step and drawing only the uncovered strips; a toroidal (wrapping) address does the same without moving anything; CryEngine re-fits near the edge; Cyberpunk redraws over several frames into a second map and swaps.
- Over 5 km a walking camera rarely re-centres; ours redraws whole when it does (2.3 ms, once).

## What it means here

**(a) with patches** (`far-shadow-movers`):

- Casters moving this frame (their matrix not last frame's) are left out of the far tile's fingerprint, so they no longer redraw it.
- A second depth image the far tile's size holds the still casters, redrawn when the camera changes cell, the sun turns or a still caster changes.
- Each frame, the patches where movers' shadows were last frame and are this frame are copied back from it, and the movers drawn over them.
- Every reader is unchanged.

**Limits:**

- The far tile's texels are 2.7 m on Ultra (5.4 m otherwise), and casters under two texels are left out of it, as before: the smallest rocks cast no far shadow; the bigger ones move in whole-texel steps.
- If that shows, add (b) only round the islands: a small finer map for the movers, its visibility taking a minimum with the far map's.
- With the sun moving (a running clock), the still copy goes stale every frame anyway and the tile is redrawn whole, as now.

**Virtual shadow maps** (phase 14) would replace the cascades and the far map; see ROADMAP.md for what they need and why they wait.

# Distant occluders in the near cascades: research

Gathered on Nov 14, 2026, after the user saw the floating islands (400-700 m up) shadow the Monolith when looking towards the sun but not when looking at it.

## The problem

- Each cascade's box takes casters only `directionalCasterReach` (200 m) towards the sun past its sphere.
- An island 500 m up is outside every cascade's box, so a point the cascades hold is lit under it; the same point read from the far shadow map (5 km, which holds the island) is shadowed.
- A reach of 2000 m fixed it, but the islands were then drawn into every cascade: shadows went from about 2 to 6 ms (the user's overlay). Rejected as a default.

## What engines do

| Approach | Who | Notes | Source |
|---|---|---|---|
| No limit towards the sun, casters clamped onto the near plane (pancaking, depth clamp) | Unity, common practice | right, but every tall caster is drawn into every cascade: our 2000 m test's cost | [Unity](https://docs.unity3d.com/2019.2/Documentation/Manual/DirLightShadows.html), [GameDev.net](https://gamedev.net/forums/topic/639036-shadow-mapping-and-high-up-objects/5034143/) |
| Pancaked depth breaks blocker-distance soft shadows (PCSS): a clamped caster looks as near as the plane | VulkanSceneGraph | why a far caster's penumbra cannot come from a pancaked cascade | [discussion](https://github.com/vsg-dev/VulkanSceneGraph/discussions/991) |
| Far cascades for flagged casters (landscape) **past** the dynamic shadow distance, not inside it | Unreal 4/5 (`bCastFarShadow`, `FarShadowDistance`) | no compositing into the near cascades | [Epic](https://docs.unrealengine.com/4.27/en-US/API/Runtime/Engine/Components/UDirectionalLightComponent/FarShadowDistance/index.html) |
| Ray-traced distance field shadows past the cascades | Unreal 4/5 | again only past the near range; with virtual shadow maps both are used | [Epic](https://dev.epicgames.com/documentation/unreal-engine/using-distance-field-shadows-in-unreal-engine) |
| One virtual map covering everything, pages cached | Unreal 5 virtual shadow maps | no reach problem at all: the real cure (our phase 14) | [Epic](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine) |
| Cached far maps, movers in the cascades | CryEngine 3, Cyberpunk 2077 | see the first half of this doc | |

No source found describes reading a coarse map's distant blockers inside the near cascades. The idea is this doc's own, built from parts the sources do describe (two maps combined by a minimum, way (b) above).

## The plan: one tap of the far map, distant blockers only

- Where a near cascade shades a point, also take **one hardware-filtered comparison** of the far map (the row's last tile), with the reference depth moved `directionalCasterReach` towards the sun.
- So only blockers more than the reach towards the sun count: exactly the casters a cascade may have left out. Nearer ones, already in the cascade, are skipped, and so is the far map's own coarse self-shadowing (its 2.7 m texels would otherwise darken every slope).
- The two are combined by a **minimum**, not a product: a caster within the cascade's box but further than the reach (a point deep inside the sphere) is in both, and the minimum does not darken it twice.
- **Softness:** the sun's disc blurs a shadow by about a hundredth of the caster's height (5 m under an island 500 m up), wider than the far map's 2.7 m texels, so one bilinear comparison is near enough; more taps if edges show.
- **Readers:** the main pass (`sunCascadeRow`), the fog (`sunShadowInAir`), the reflection probe's capture (the same shader library).
- **Cost:** one texture read per pixel in the cascades, about 0.1-0.3 ms (to be measured). No extra drawing: the far map is drawn already.
- **Limits:** a caster under two far texels (5.4 m) is not in the far map, so small things more than 200 m up cast nothing in the cascades (as now). With the far map off, nothing changes.
