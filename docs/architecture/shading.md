# Shading

Covers: `src/render/gpu/brdf`, `src/render/gpu/normal_mapping`, `src/render/gpu/material_settings`, `assets/shaders/brdf.glsl`, `assets/shaders/normal_mapping.glsl`, `assets/shaders/scene_surface.glsl`

How a lit surface answers each light in the main pass: the physically based model, its table, normal maps and their filtering, clear coats and light through leaves. Lights themselves: `lights.md`. Height maps and blended grounds: `parallax-and-grounds.md`. The mirror rule for shader maths: `.claude/rules/shader-mirror.md`.

## The model (`render/gpu/brdf`, `brdf.glsl`)

glTF metallic-roughness. Numbers in `render/gpu/brdf_shared.h`: dielectric reflectance 0.04, smallest roughness 0.045, smallest view cosine, table size 32.

**Per light** (`shadeLight` / GLSL `addLight`):

- Specular: GGX distribution × Smith height-correlated visibility × Schlick Fresnel, times the multiple-scattering compensation `1 + f0 (1 / E - 1)` (Turquin 2019; E from the table).
- Diffuse: Lambert × `1 - specular albedo`, so a white surface under even light returns exactly what it gets (the white furnace test).
- **Units:** the BRDF is multiplied by π, so a light's radiance is the irradiance it gives a surface facing it, and white matte lit head-on by 1 shows 1.
- `facesLight` is asked before a light's shadow lookups: a light giving nothing would waste them.
- Directional lights arrive dimmed by the fog above the surface (`fogSunThrough`, `fog.md`, Layers).

**`ShadingPoint`** (`prepareShading`, once a pixel, C++ and GLSL alike):

- `f0 = mix(0.04, baseColour, metallic)`, alpha = roughness² (roughness clamped to the smallest).
- From the table: the compensation, specular albedo and the diffuse colour already weighted by what the specular leaves.
- Optional layers added after: `addDiffuseTransmission`, then `addClearCoat`.

**Light from all around:**

| Function | Used for |
|---|---|
| `shadeAmbient` | flat ambient: `ambient × (diffuse + specularAlbedo)` (+ a coat's albedo), so metals are not black |
| `shadeProbeAmbient` | probes' or sky grid's diffuse light plus a mirror-direction stand-in for the specular; kept apart so pictures without probes stay bit-identical |
| `shadeTransmittedAmbient` | light behind a surface that lets light through |

## The specular albedo table

- `BrdfTable`: scale and bias for f0 over n·v and roughness, 32 × 32, texel i at i / (size - 1).
- `buildBrdfTable(jobs)`: GGX importance sampling on a Hammersley sequence, `brdfTableSamples` (4096) a texel, one texel a job; the same bits on any thread count.
- `brdfTableTexels` → `R16G16_UNORM`, uploaded by `VulkanRenderer::createFixedTextures`; scene set 1 binding 9 (`sampleBrdfTable` in `scene_surface.glsl`).

## Textures in the main pass

- A material's colour, metallic-roughness, normal, occlusion and emissive textures (and the coat's, and glass's) come from the bindless array (set 0) by the slots in its `GpuMaterial` (GPU scene).
- `GpuFrameData::textureOptions`: x the mip bias (upscaling), y Toksvig on, z the geometric specular variance (0 off), w clear coats on. `materialOptions`: parallax (`parallax-and-grounds.md`) and w light through leaves.

## Normal mapping (`render/gpu/normal_mapping`)

Functions: `decodeNormalTexel`, `perturbNormal`, `transformTangent`, `toksvigRoughness`, `geometricRoughness`; GPU cases in `normal_mapping_gpu_test`.

- Tangents: `MeshVertex::tangent` (MikkTSpace or the file's). `triangle.vert` carries it with the model matrix and multiplies its sign by `handedness(model)` (`math/mat4`, stored in `GpuInstance`), so mirrored objects keep their bumps.
- Decode: `(2c - 1) · (scale, scale, 1)`; bitangent `w · (N × T)` from the interpolated, unnormalised normal and tangent (MikkTSpace's rule); only the sum is normalised.
- Shadow offsets use the geometric normal.
- The normal map is read once, before the roughness, outside any branch.

**Normal map filtering** (Toksvig 2005; `RenderSettings::normalMapFiltering`, on):

- Mips are averaged and never renormalised, so a mip texel's length L (decoded without the scale) measures how much the bumps beneath disagree.
- L is first lengthened by `NORMAL_TOKSVIG_QUANTIZATION_SLACK` (√3/255, what 8-bit rounding alone removes) and capped at 1.
- The spread `(1 - L) / L` × scale² is added twice to alpha², capped at roughness 1. A full-size texel gives the material's roughness back exactly.

**Geometric specular anti-aliasing** (Tokuyoshi and Kaplanyan 2019; `RenderSettings::specularAntiAliasing`, on, and `specularAntiAliasingVariance`, default `NORMAL_GEOMETRIC_VARIANCE` 0.25; F5 Passes):

- A pixel on a curved surface covers a range of normals and should show the highlight averaged over them; otherwise a sub-pixel highlight (a smooth ball's sun) flickers with every tiny camera move and TAA jitter.
- `scene_surface.glsl` takes the smooth vertex normal's screen derivatives first thing (before any branch or discard); after Toksvig, `geometricRoughness` adds twice their squared lengths × the variance to alpha², at most `NORMAL_GEOMETRIC_LIMIT` (0.18).

## Clear coat (`ClearCoatProperties`, `addClearCoat`)

A clear varnish over a base (car paint, lacquered wood), as `KHR_materials_clearcoat` and Filament describe it.

- **Lobe:** a second GGX lobe for a 4% reflector at the coat's roughness and normal, × its amount, with its own compensation (`coatCompensation`).
- **Energy:** `coatAlbedo` = amount × the table's albedo for a 4% reflector at its roughness and view; the base's diffuse, specular albedo and compensation are × `1 - coatAlbedo`, so a coated white surface still passes the furnace. glTF dims by Fresnel at the view angle instead, slightly off for rough coats; the table is exact at no extra cost. Taken once a pixel at the eye's angle.
- **Normal:** the geometry's, not the normal map's (varnish fills the bumps), unless the material has `clearCoatNormalTexture`.
- Each layer is lit only from in front of itself; `facesLight` says yes if either is.
- Ambient: the coat adds its albedo of the light from all around and, with probes or the sky grid, of the mirror stand-in.
- A coat of nothing leaves every result bit-identical.

**In the main pass** (not glass's pipelines), while `MaterialSettings::clearCoat` is on (F5 Materials, `engine.render.materials.clearCoat`) and the material has a coat:

- The coat's three textures are read with `textureGrad` at UV steps taken first thing (× the mip bias), so uncoated pixels read nothing more.
- Its roughness is clamped, then widened by Toksvig (its own normal map) and geometric specular anti-aliasing, as the base's: smooth coats on curved bodies are where highlights shimmer most.
- Every light goes through `addLight`, so sun, point, spot, mirrored lights and the reflection probe's capture all see it.

**Reflections** follow one layer a pixel (two would double the cost): `reflectsCoat` picks the smoother (the coat on a tie), whose mirror image is the sharper. The main pass then hands on the coat's normal, grey albedo, roughness and stand-in; the other layer keeps its stand-in.

Test scene: the light lab's two red balls, one bare and one lacquered (`LabSurface::BallPaint`, `LacqueredPaint`, `labPaintedBallCentres`).

## Light through leaves (`addDiffuseTransmission`)

Diffuse transmission, as `KHR_materials_diffuse_transmission` and Unreal's two-sided foliage: a leaf scatters light many times inside, so light reaching its far side leaves the seen side evenly in all directions; a backlit leaf glows from any view. Research: `docs/research/JUNGLE.md`.

- `ShadingPoint::transmitted` = diffuse colour × `Material::diffuseTransmissionTint` (linear) × `Material::diffuseTransmission` (the share; about 1 for a leaf). Reflection is unchanged, so the lit side looks the same. glTF splits the diffuse colour between sides instead; its numbers map onto these.
- Cut per channel so diffuse + specular albedo + transmitted ≤ 1 (the furnace from both sides).
- A light behind (n·l < 0) adds `transmitted × radiance × -n·l` to the diffuse, no highlight (`transmitsLight`, `facesLight`). Applied before `addClearCoat`, which dims it with the base. A share ≤ 0 changes nothing to the bit.

**In the main pass** (every pipeline but glass's), while `MaterialSettings::lightThroughLeaves` is on (F5 Materials) and the material's share (`GpuMaterial::diffuseTransmission`; none on transparent materials) is above 0:

- Shadows: the lookup point lifts along the geometric normal turned to the light's side (`shadowLiftNormal`), else a backlit leaf reads its own shadow; contact shadows are skipped for light coming through (`throughSurface`), since their march would hit the leaf.
- Light from behind: the flat ambient, the sky grid or the probes read along the reversed normal (diffuse only), dimmed by the same ambient occlusion.
- Only the main pass: ray hit lighting (`ray_hit_lighting.glsl`), probe rays and the impostor bake light leaves from the front only.

The forest sets every leaf's share and tint through `withLeafLight` (`ForestLeafLightSettings`, start-up only).

## Tests

- `brdf_test`: the furnace (plain, coated, transmitting), a nil coat and nil share changing nothing, the coat's share and lobe, its normal, which layer reflections follow, light from behind, the shadow lift and contact skip.
- `normal_mapping_test`, `material_uniforms_test`, `engine_commands_test` (the switches by command); GPU cases in `brdf_gpu_test` and `normal_mapping_gpu_test`.
