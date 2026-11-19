# Fog history and fast lights

Research for the froxel fog's trail behind a moving flashlight (`docs/architecture/fog.md`, History: `historyWeight` 0.9 keeps 90 % of last frame's light, so a moving or switched light settles over about 10 frames). [V] read in a primary source (docs, shader source, slide text), [S] secondary or a search snippet, [K] background knowledge, not checked.

## What shipped engines do

| Engine | History weight | Rejection | Clamp | Fast lights |
|---|---|---|---|---|
| Unreal 4/5 | 0.9 (`r.VolumetricFog.HistoryWeight`) [S] | outside last frame's grid; 4 light samples on a miss (`HistoryMissSupersampleCount`) [S] | none [K] | docs: flashlights and muzzle flashes "leave lighting trails"; set the light's Volumetric Scattering Intensity to 0 ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine)) [V] |
| Frostbite 2015 (Hillaire) | 0.95 [S] | outside the frustum [S] | none; "moving lights leave trails", "use neighbour clamping?" listed as open ([slides](https://www.slideshare.net/DICEStudio/physically-based-and-unified-volumetric-rendering-in-frostbite)) [S] | none |
| Unity HDRP | 6/7 = 0.857 (7 frames) [V] | invalid history [V] | none | a TODO in `VolumetricLighting.compute`: dynamic lights "should neither read nor write to the history buffer" ([source](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/VolumetricLighting/VolumetricLighting.compute)) [V] |
| Godot 4 | 0.9 [K] | outside the frustum, NaN [V] | none | docs: moving lights ghost; slow them, lower the amount, or fog energy 0 on short-lived lights ([docs](https://docs.godotengine.org/en/stable/tutorials/3d/volumetric_fog.html)) [V] |
| CryEngine 5 | a blend factor: "less flicker, but more ghosting" [V] | - | an "Advanced" mode: "less ghost artifact but slightly more flicker", internals not public ([docs](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/26215326)) [V] | none |
| AC IV (Wronski 2014) | reprojection only as a later extension; only out-of-frustum data invalid ([PDF](https://bartwronski.com/wp-content/uploads/2014/08/bwronski_volumetric_fog_siggraph2014.pdf)) [V] | - | - | - |

- Epic's forum answer: the reprojection "is needed to make the technique work with reasonable performance"; users ship 0.5 (no trails, more flicker) ([forum](https://forums.unrealengine.com/t/volumetric-light-ghost-trail-issue-still-no-fix-for-it/492710)) [S].
- The only published per-froxel clamp: a Gothenburg master's thesis built in Frostbite, variance clipping of the froxel history with anti-flicker heuristics, the 6 face neighbours the best balance; exponential averaging alone "performs poorly under dynamic volumetric lighting" ([thesis](https://gupea.ub.gu.se/items/e05613c2-e1d4-48b0-b41e-c32f4b0ecbae)) [S]. Variance clipping: Salvi, GDC 2016 [S].
- Nothing documented found for RDR2, Decima, Call of Duty, id Tech or Cyberpunk 2077.

## Options for us

| Option | How | Cost grows with | Weakness |
|---|---|---|---|
| C. Fast lights outside the history | the history keeps only slow lights; lights flagged `fast` evaluated fresh each frame into a grid of their own, added before the march | froxels (a second grid, 3.7-7.4 MB) plus fast lights' shading; estimated under 0.05 ms [K] | their jitter noise is not averaged: sample the froxel's middle, or 2-4 samples as Unreal does on a history miss |
| A. Neighbourhood clamp | variance clipping over the 6 face neighbours, current frame written first, then a resolve | froxels x 7 reads; estimated 0.05-0.15 ms [K] | one jittered sample a froxel makes the range wide and noisy; the thesis needed anti-flicker heuristics |
| B. Weaker history where it changed | lower the weight where current and history differ | froxels | jitter noise triggers it everywhere: flicker, as at weight 0.5 |

- Shipped practice: a fixed weight near 0.9 and trails accepted; the documented fix is to take fast lights out of the fog. The evidence supports C for lights flagged fast (the flashlight already is), A later for other moving lights (searchlights, sweeping floods).
- A refinement to try within C [K, not from a source]: a camera-held light barely moves in view space, so a history of its own reprojected in view space, blended near 0.5, would stay valid while the camera turns.
