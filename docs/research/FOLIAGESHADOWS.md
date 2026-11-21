# Plants' shadows near the camera: research

Phase 18, ROADMAP e1c 4c: a plant casts from a shadow stand-in (about 5% of its triangles); up close the stand-in's flat
triangles poke out round the seen leaves and shadow them in hard polygons. The plan was to cast from the seen mesh in the
nearest one or two cascades.

## What engines do

No shipped source uses a coarse proxy right next to the camera. The seen level casts near, and cheaper ones further out.

| Who | Near the camera | Further out | Source |
|---|---|---|---|
| Horizon Zero Dawn (GDC 2018, Sanders) | Plants cast into cascades 0-1 only; every object in cascade 1 picks its level as if 25 m away, held while it stays in the cascade ("almost completely solved shadow popping"); casters unanimated | A height field shadows the distance | gdcvault.com/play/1025530 |
| Arma 3 vegetation | `VisualEx`: each seen level casts its own; a shadow level "should match the first resolution level as closely as possible" | `explicit` shadow levels | community.bistudio.com/wiki/Arma_3_Vegetation_P3D |
| Unreal (CSM) | Shadow level by distance, `r.Shadow.LODDistanceFactor` 1: about the seen level | `r.Shadow.FarShadow.LODDistanceFactor`; wind cut off by distance for virtual shadow maps | ue544cvarwiki |
| Unity + SpeedTree | Cast shadows on per level (example: first two on, third off); billboards turn to the light | | docs.unity3d.com SpeedTreeImporter |
| CryEngine | Shadow proxies: a hidden low-poly child casts (10,218 -> 1,890 triangles), the seen mesh "No Shadow" | `e_ShadowsCacheObjectLod` for cached far maps | cryengine.com docs |
| An Unreal SpeedTree setup (Winter Crown) | A 2-triangle shadow card casts; the seen mesh keeps contact shadows; "not fully accurate" accepted | | winter-crown-works.com/en/tech/004 |

- No name for the artefact; it is "erroneous self-shadowing". The same shows during level cross-fades (gamedev.net: shadows in the middle of coarser polygons).
- Mitigations seen: the seen level casts near (HZD, Arma, Unreal); screen-space contact shadows from the seen mesh fill close detail; cut-out cards cast their alpha shape.
- Cost: no controlled measurement found. HZD: "increases render cost somewhat… definitely worth it", shadows 6-9% of its frame. One Unreal forest: shadow depths 10.5 ms, 6.5 ms with every tree removed.

## What it means here

- Our cascades end near 3 / 9 / 36 / 200 m, so seen meshes in cascades 0-1 cost what the plants within about 9 m cost, not the forest's (cost grows with what is near).
- Levels by the tile's own texels, held while the camera walks (already ours: `tileLevelView`), is HZD's anti-pop rule.
- Seen meshes sway; the nearest cascade already redraws every frame while things sway, so a swaying seen caster costs nothing extra there.
