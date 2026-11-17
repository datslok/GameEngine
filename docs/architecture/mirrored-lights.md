# Mirrored lights

Covers: `src/render/gpu/mirrored_light`, `src/render/gpu/mirrored_light_shared.h`, `src/render/vulkan/vulkan_mirrored_light`, `assets/shaders/mirrored_light.glsl`, `assets/shaders/mirrored_light_search.glsl`, `assets/shaders/mirrored_light.comp`, `assets/shaders/ddgi_mirror.comp`

Light reaching a surface by one reflection off a shiny one (a flashlight shone into a mirror lighting the floor: a caustic). Direct light follows straight lines and probe rays see a metal mirror as black (no matte colour), so neither carries it. Two ways: with rays, a per-pixel search (manifold next event estimation); without, image lights for flat mirrors. Mirror rules: `.claude/rules/shader-mirror.md`.

## Which way a frame uses

| Way | When |
|---|---|
| The search | `tracesRays` (the master switch, `ray-tracing.md`), `MirroredLightSettings::enabled`, depth prepass on, and pairs found |
| Image lights | the search is off for any of the first three reasons, and `imageLights` on |

## Settings

`MirroredLightSettings` (`RenderSettings::mirroredLights`, F5 Reflections, keys `engine.render.reflections.<field>`):

| Setting | Default | Note |
|---|---|---|
| `enabled` | on | the search; Ultra only, off below |
| `bounce` | on | the light probes bounce it on (`ddgi_mirror.comp`) |
| `mirrorRoughness` | 0.25 | surfaces up to this reflect lights, fading over `MIRROR_ROUGHNESS_FADE` (0.05) more (`mirrorRoughnessFade`); past `MIRROR_FOOTPRINT_ROUGHNESS` (0.1) from a footprint |
| `imageLights` | on | image lights whenever the search does not run |

## Pairs

`chooseMirrorPairs` (CPU, each frame): each fast light (`GpuLights::fastPoints`, `fastSpots`: the flashlight, the demo's flying lamps) with each shadow-casting, not transparent draw whose roughness fade is above 0 and whose bounding sphere its range (and a spotlight's cone, `coneTouchesSphere`) reaches; nearest first by the gap between light and sphere; at most `MIRROR_MAX_PAIRS` (16).

- Found from materials: nothing is flagged.
- No pairs: no pass; the frame is drawn exactly as without.
- Scene set 1 binding 24 (`GpuMirrorPairs`, `packMirrorPairs`): count, roughness limit, each pair's light kind and slot, draw and camera-relative bounds; count 0 when off. Every user reads the same pairs.

## Who runs the search

`mirroredLightAt` (`mirrored_light_search.glsl`), over the same pairs:

| Where | Lights | Footprint samples |
|---|---|---|
| `mirrored_light.comp` | each pixel, for the main pass | `MIRROR_FOOTPRINT_SAMPLES` (4) |
| `ddgi_mirror.comp` | probe fast rays' hits, into the fast atlas | `MIRROR_HIT_FOOTPRINT_SAMPLES` (1) |
| `probe_capture_rays.frag` | the reflection probe's traced capture | 1 |
| `reflections_rays.comp` | traced reflections' hits (`mirroredLightAtRayHit`) | 1 |

`ray_hit_lighting.glsl` compiles `mirroredLightAtRayHit` only with `RAY_HIT_MIRRORED_LIGHTS`, since its registers slow every ray of a shader that holds it, used or not.

## The search

Per pixel and pair, from the receiver x (from the prepass depth) to the light L by a point y on the surface (Hanika, Droske and Fascione 2015):

- **Constraint** (`mirrorConstraint`): the half vector h = unit(x − y) + unit(L − y) lies along the normal, so its tangent components (`tangentFrame`, Duff et al. 2017) are zero.
- **Jacobian** over tangent moves d (`mirrorJacobian`): J d = −T P d − (h·n) S d, with P = (I − ωx ωxᵀ)/dx + (I − ωL ωLᵀ)/dL and S the shape operator (dn = S dy).
- **Curvature** (`triangleCurvature`): the interpolated normal changes linearly over the hit triangle, so S follows from the edges and corner normals' differences (least squares through the edges' Gram matrix), over the interpolated normal's length; 0 when flat, about 1/R on a sphere mesh.
- **First guess:** a ray from x (lifted towards the camera by `MIRROR_RAY_OFFSET` + `MIRROR_RAY_OFFSET_PER_METRE` per metre) towards the pair's sphere centre; it must hit the pair's draw first, from the front (mask `RAY_MASK_SOLID`), which also proves x's way clear.
- **Flat step** (`planeMirrorPoint`): where the line to L's image behind the tangent plane crosses it; exact for a flat mirror.
- **Flat early end** (`isFlatMirror`: curvature under `MIRROR_FLAT_CURVATURE`): a step that misses the surface, or lands on a face not in its plane (normals further apart than `MIRROR_SAME_PLANE_COSINE`, 0.999: a thin mirror's rim), ends the search off the mirror (`MirrorPath::onMirror` false), the point kept on the mirror's plane: a sharp mirror gives nothing there, a rough one's footprint may still reach the mirror. Without it, pixels reflecting past the mirror's edge crept towards it for dozens of rays.
- **Receiver on the plane** (within `MIRROR_RAY_OFFSET`: the mirror itself): nothing.
- **Newton** (`mirrorNewtonTarget`: y − J⁻¹ c, cast back onto the surface by a ray from x), each step halved up to `MIRROR_BACKTRACKS` (4) times while it falls off, at most `MIRROR_MAX_STEPS` (8), until |c| ≤ `MIRROR_SOLVE_TOLERANCE` (0.001), with x and L both in front. CPU mirror: `solveMirrorPath` with a `MirrorProjection`.
- **Visibility:** a shadow ray from y (lifted along its normal) to the light, after a spotlight's cone, tinted by glass (`lightReachesMirror`, `lightThrough`, per channel); the light leaving the mirror passes `glassTransmittance` towards x along the path's centre, the footprint's samples sharing it. Both legs are linear in the light, so a rough mirror's tinted mean goes in with the intensity.

## Brightness

Intensity I sends I dΩ into the solid angle round its ray to y, landing on area dA at x across the ray: irradiance I dΩ/dA.

- **Spread area** (`mirrorSpreadArea`): implicit differentiation of the constraint gives dA/dΩ = dx² dL² |det J| / cos²θ.
  - Flat mirror: (dx + dL)², the image's squared distance.
  - Ball of radius R head-on: (dx + dL + 2 dx dL / R)² (the mirror equation).
  - Concave mirror: 0 at its focus.
- **Irradiance** (`mirroredIrradiance`): I × cone × Fresnel × `rangeWindow`(dx + dL)² / (area + sourceRadius²). A flat mirror gives exactly the light's image with the engine's falloff, and a focus stays finite (no surface lit brighter than by the source filling its sky).
- **Reflectance:** Schlick's Fresnel from the mirror's material at the hit (base colour and metallic, read as the main pass reads them) at the angle of incidence, times the roughness fade.

## Rough mirrors

Past `MIRROR_FOOTPRINT_ROUGHNESS` (brushed steel, polished stone; `usesMirrorFootprint`): facets round y tilt, so a footprint round it sends the light on.

- **Integral:** D F G / (4 cos θi cos θo) turns the light into I F G / (4 dx² dL²) × ∫ D(half vector) over the mirror (the cosines cancel). With D Gaussian in slope space (Beckmann, alpha = roughness² as GGX, same peak) and the half vector's slope J d / a for a step d (a = h·n), the integrand is a Gaussian on the mirror with covariance s² (JᵀJ)⁻¹, s² = a² alpha² / 2 + (emitterRadius / dL)² / 4. The bulb's size is `emitterRadius` (as soft shadows), not `sourceRadius` (the falloff's dial, large on the flashlight).
- **On a mirror bigger than the footprint the integral equals the sharp answer:** roughness softens the spot's edges (the mirror's, a cone's, shadows), it does not dim it.
- **Estimate** (`footprintLight`, `roughMirroredIrradiance`): `MIRROR_FOOTPRINT_SAMPLES` points a frame from a Gaussian of precision JᵀJ / s² + I / R², R the mirror's bounding radius, so a focus's footprint (det J = 0) stays on the mirror and finite.
  - `gaussianSamplePoint`: stratified radii sqrt(−2 ln(1 − u)), evenly spread angles, turned each frame by interleaved gradient noise of the pixel and the frame (`GpuFrameData::mirroredLightOptions` y: the TAA frame mod 1024), so samples are exactly normal over the frames; TAA averages them.
  - Each point is put on the mirror by a ray from x (`projectOntoMirror`, proving it visible), lit by `lightReachesMirror`, weighted by true over drawn density (`footprintWeight` = 1 / (s² sqrt(det P)) exp(|d|² / 2R²); 1 / |det J| on a big mirror).
  - `mirrorFootprint` computes det P as det(J)² / s⁴ + tr(JᵀJ) / (s² R²) + 1 / R⁴, not from the Cholesky factor's last entry, which a near-singular J cancels away.
  - Light: I F `rangeWindow`² a² / (4 dx² dL²) × the mean weight.
- **Off a flat mirror's edge** the point stays on its plane (`onMirror` false), so the soft edge reaches past the sharp one.
- **Dropped:** GGX's long tail, the facets' shadowing G, and F, distance and the slope's 1 / cos⁴ varying across the footprint: under 2% at the roughness used, by the test against the exact sum.

## The pass

`VulkanMirroredLights::light`, timed as MIRROR (`GpuPass::MirroredLights`), after the depth prepass and ambient occlusion, before the main pass:

- One invocation per render pixel, `MIRROR_GROUP_SIDE` (8) square, from each pixel's point lifted towards the camera.
- Set 3 after the scene's two sets and the ray tracing set (`MirroredLightSets`): `GpuMirroredLightPass` (the camera-relative inverse view-projection with jitter, the size), the depth (exact texels), and two render-size outputs: the light arriving (RGBA16F) and its direction (RG16F, `octahedralEncode`), the pairs' directions blended by brightness (exact for one pair).
- Barriers: depth from the prepass to compute reads (the main pass turns it back into a depth target); outputs from last frame's fragment reads to storage writes, contents discarded, then to fragment reads.
- Images made the first time the pass runs, remade with the frame's size after.

**Main pass:** reads them at its own pixel (scene set 1 bindings 22 and 23, `MirroredLightBinding`, `MirroredDirectionBinding`; a stand-in texture when the pass did not run) while `mirroredLightOptions` x is 1 (not in captures), and adds them with `addLight` from the stored direction, so matte and glossy surfaces both respond.

**Traced capture and reflections:** `probe_capture_rays.frag` runs the search at each texel from the point lifted along its normal and adds it the same way; traced reflections add `mirroredLightAtRayHit` to their hits. Screen-space reflections already show it where it is on screen.

## The bounce

`ddgi_mirror.comp`, a pass of its own right after the probe trace, while there are pairs, a listed fast light and `bounce` on:

- The trace's layout, sets and push constant. Only rays of probes a fast light reaches (`probeNearFastLight`, written by the trace) that met a front face are traced again to find their surface.
- Its light: `mirroredLightAtRayHit` from the hit lifted by `DDGI_SHADOW_RAY_OFFSET`, times the cosine to its direction, on the matte colour, added to `fastProbeRays`.
- Barriers: ray buffers from the trace's writes to this pass, then to the update's reads.
- Separate from the trace because the search's registers slowed every probe ray, run or not.

Ray hits are lit matte only, with `matteColour` (`render/gpu/brdf`: base colour × (1 − metallic), the main pass's diffuse part), so mirrors and chrome are dark to probe and reflection rays, as they should be.

## Image lights (without ray tracing)

For a flat mirror the light arriving by way of it is exactly a light at the source's **mirror image**, behind the glass, shining out through the mirror's outline and dimmed by its reflectance: the path L → M → P has the image's distance |L′P| (exact falloff), and reflection keeps angles (a cone carries over). Each image is one more spotlight, so the clusters, the main pass, both captures and the fog take it with no code beyond the window test.

- **Choosing** (`chooseImageLights`, after ranking, from the drawable lights so source slots are `packLighting`'s): each point and spot light (any, not just fast) in front of a flat face (`DrawItem::flatFaces`) of a shadow-casting, not transparent metal draw (metallic ≥ 0.5) passing `mirrorRoughness` and its fade, its range (and cone) reaching the face's circle. Score: intensity × area × cosine / distance² × the mirror's luminance and fade; the best `MIRROR_IMAGE_LIGHTS` (4), ties in a fixed order. Never an image of an image.
- **Window** (`mirrorFaceWindow`): only faces square to their mesh's axes (boxes, sheets). `LightWindow` (`scene/lighting.h`, `PlacedSpotLight::window`): plane, middle, edges and half lengths, source kind and slot.
- **The image** (`imageOfSpotLight`, `imageOfPointLight`): source mirrored (`reflectAcrossPlane`), colour × `mirrorReflectance` (`reflectanceAtNormal`: the metal's colour), intensity × `mirrorRoughnessFade`, not fast, emitter radius widened by 2 roughness² × the source's distance from the mirror (soft shadows on rough mirrors). A spotlight's beam is reflected with its cone; a point light's image aims at the window's middle with an inner cone just taking in its corners (1 mrad spare, outer 0.02 rad wider).
- **On the GPU:** `GpuSpotLight` (128 bytes) carries the camera-relative window (`windowPlane`, xyz zero for a plain light; `windowCentre`; `windowSource`, a spotlight's slot + 1 or minus a point light's; `windowAxisU`/`V` with half lengths in w).
- **Lighting** (`throughMirrorWindow`, mirrored): the line from the image to the point crosses the plane inside the outline, image behind and point in front; the crossing is the reflection point M. `addSpotLight` (`scene_surface.glsl`) and the fog's `spotLightInAir` light nothing else, and multiply by the source's own shadow read at M (`imageSourceShadow`, off the mirror by the tile's normal offset; the fog by `MIRROR_IMAGE_SHADOW_LIFT`).
- **Excluded:** contact shadows never march towards an image (the screen's depth runs into the mirror); glass shadow rays stop at M; probe hit lights leave images out.
- **Shadows mirror to receiver:** the image's own spotlight tile, drawn from the image and clipped at the window's plane lifted by `PLANAR_CLIP_LIFT` (`ShadowTile::clipPlane`, `planarClipPlane`), since from the image the first thing seen is the wall behind the glass. `GpuShadowView` (112 bytes) carries the plane; `shadow.vert` and `shadow_cut_out.vert` write `gl_ClipDistance` (`planarClipDistance`, world space); other tiles get `noClipPlane`. The shadow cache tells tiles apart by it.
- **Left out:** curved mirrors (no single image), Fresnel's change with angle, a rough mirror's soft beam edge, glass on the way.
- F4's lights line shows `IN MIRRORS n` while there are any (`RenderStats::imageLights`).

## Tests

- `mirrored_light_test`: tangent frame, curvature, the flat step; a flat mirror is exactly the image light; off the edge, the rim, behind, and the flat early end; the ball obeys the law of reflection, its Jacobian matches finite differences, its head-on spread area, and the spread area against 2.25 million light rays off an exact sphere; focus stays finite; roughness fade; Gaussian samples; footprint shape; rough mirrors match the sharp light away from edges and soften the edge against the exact Beckmann sum; pairs; packing.
- `image_light_test`: face windows, the window test, a spotlight's image matching the search's flat-mirror light exactly, the point light's cone, choosing and the cap, the shadow tile's clip plane and the cache, packing.
- `brdf_test` (`matteColour`), `gpu_scene_test` (the shadow view's plane), `light_lab_test`.
- GPU: `mirrored_light_gpu_test` (footprint functions and `throughMirrorWindow` too), `brdf_gpu_test`.
