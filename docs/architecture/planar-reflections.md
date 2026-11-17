# Planar reflections

Covers: `src/render/gpu/planar_reflection`, `src/render/gpu/planar_reflection_shared.h`, `src/render/vulkan/vulkan_planar_reflection`, `assets/shaders/planar_reflection.glsl`, `src/scene/flat_faces`

A flat mirror's picture drawn for real: the scene drawn a second time through the reflection. The reflection probe sees from one point, so a head-on mirror, which shows at full size what the probe never saw (past a door frame), looks jagged at any probe size. The reflection pass: `reflections.md`. Mirror rules: `.claude/rules/shader-mirror.md`.

## When

- While reflections run (anti-aliasing on, `ReflectionSettings::enabled`), `planarMirrors` > 0, and not with ray-traced reflections, which are exact anyway.
- At most `PLANAR_MAX_MIRRORS` (1) plane a frame.
- Timed as PLANAR (`GpuPass::PlanarReflection`), after CUBE, before SSR.

## Choosing the mirror

`choosePlanarMirror`, each frame from the camera's visible draws; nothing marked by hand:

- Candidates: metal (metallic at least 0.5), roughness at most `planarMaxRoughness`, not transparent; their flat faces (`DrawItem::flatFaces`, found at load by `scene/flat_faces`) facing the camera.
- Score: face area × cosine to the camera / distance³ (the solid angle it fills).
- Last frame's plane (`VulkanRenderer::lastMirror`) keeps its place unless another scores half as much again, or mirrors would pop as the camera moves.
- Everything on the chosen plane is the mirror, so panels side by side on a wall share one picture.
- `MirrorPlane`: n·x = offset, normal towards the reflected side; `planeRelativeTo` measures it from the camera.

## The view

- **Projection** (`planarViewProjection`): the camera's view-projection without jitter times the reflection (`planeReflection`: x → x − 2 (n·x − d) n). What stands in front of the mirror lands on the very pixel where the eye sees it in the mirror, so the picture is read at the mirror's own pixel with no search. Depths are the camera's own, reflected, so the depth measurement reads them as it reads the main pass's.
- **Culling** (`planarFrustum`): the view with the mirror's plane as its near side, so nothing behind the glass is drawn. Its own level-of-detail view and card thinning error (`mirrorThinning`).
- **Clipping at the glass:** every pipeline clamps depth rather than clip at the near plane, so an oblique near plane (Lengyel 2005) cuts nothing and flattens the wall behind the mirror over the picture. Instead `triangle.vert` writes `gl_ClipDistance[0]` (device feature `shaderClipDistance`): `planarClipDistance` from `GpuCaptureView::clipPlane` (`planarClipPlane`, lifted `PLANAR_CLIP_LIFT`, 2 mm, towards the camera so the mirror's own face does not flicker in its picture). Every other view gets `noClipPlane`.
- **Pipelines:** the reflection probe's capture pipelines (a reflection turns triangles round, as cube faces do, so front faces are culled); draws listed after the probe's faces (`captureDraws[cubeFaceCount]`).
- **Sky:** over the background through the camera's rays mirrored (`mirroredRays`), discs kept.
- **Eye:** `GpuCaptureView::eye` is the camera reflected across the mirror (`reflectAcrossPlane`, camera-relative), handed to the capture shaders as `viewerPosition`, so view-dependent shading (highlights, the sky visibility lookup's shift towards the viewer) uses the direction the mirror shows; with the real camera, a wall between camera and mirror was looked up from its back and glowed as open sky.
- **Sun shadows:** the mirror shows mostly what the camera's cascades never hold, so it gets cascades of its own (`mirroredCamera`, `mirrorCascadesStart`, `MirrorShadowView`; `sun-shadows.md`, A mirror's cascades).

## The picture

`VulkanRenderer::drawPlanarReflection` into `VulkanPlanarReflection`:

| Image | Size | Use |
|---|---|---|
| Picture | `planarRenderSize` (render size × `planarScale`, at least a texel), scene format | cleared to the background |
| Depth | same | kept and measured (`mirrorDepthRange`) for the mirror's cascades |
| Levels | `VulkanColourPyramid` | ever blurrier, for rough mirrors |

## Reading it

In the reflection pass (`reflections_pass.glsl`):

- On the mirror (`isOnPlanarMirror`): within `PLANAR_PLANE_TOLERANCE` (1 cm) plus `PLANAR_PLANE_TOLERANCE_PER_METRE` (5 mm a metre, for depth's rounding) of the plane, normal within `PLANAR_NORMAL_COSINE` (0.98) of its normal.
- There the picture, at the pixel's place without this frame's jitter and at `probeRoughnessLevel` for its roughness, replaces the probe's light, trusted by the roughness fade; screen-space reflections are skipped.

## Settings

In `ReflectionSettings`, F5 Reflections:

| Setting | Default | Note |
|---|---|---|
| `planarMirrors` | 1 | 0 or 1 |
| `planarMaxRoughness` | 0.4 | the picture's blur is the same everywhere, where true blur grows with distance from the mirror |
| `planarScale` | 0.5 | share of the render size |

The mirror's cascades: `ShadowSettings::mirrorCascadeCount` (`render/gpu/shadow_map.h`).

## Limits

- No ambient occlusion or gathered bounce light on the mirror's surfaces (both come from the screen).
- A mirror seen in the mirror shows only its stand-in.
- Curved mirrors: the probe or ray tracing.

## Tests

- `planar_reflection_test`: flat faces, the reflection, the view landing on the mirror's pixel with ordinary depths, the mirrored camera and where its cascades start, choosing and keeping a mirror, the on-mirror test, mirrored sky rays, the clip plane, the frustum, the picture's size.
- `gpu_scene_test`: the capture view's push constants.
- `reflection_probe_gpu_test`: `isOnPlanarMirror`, `planarClipDistance`.
